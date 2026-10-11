// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 *
 * Qualcomm PM8150B Fuel Gauge (FG-GEN4) driver.
 */

#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#include "qcom_bms.h"

#define BATT_MONOTONIC_SOC 0x009
#define FULL_SOC_RAW 255
#define FULL_SOC_REPORT_THR 250

#define PARAM_ADDR_BATT_VOLTAGE 0x1a0
#define PARAM_ADDR_BATT_VOLTAGE_CP 0x1a6
#define PARAM_ADDR_BATT_CURRENT 0x1a2
#define PARAM_ADDR_BATT_CURRENT_CP 0x1a8

#define BATT_VOLTAGE_NUMR 122070
#define BATT_VOLTAGE_DENR 1000
#define BATT_CURRENT_NUMR 488281
#define BATT_CURRENT_DENR 1000

#define BATT_INFO_BASE 0x100
#define MEM_IF_BASE 0x300
#define MEM_IF_INT_RT_STS (MEM_IF_BASE + 0x10)
#define MEM_INTF_CFG (MEM_IF_BASE + 0x50)
#define MEM_IF_DMA_STS (MEM_IF_BASE + 0x70)
#define MEM_IF_DMA_CTL (MEM_IF_BASE + 0x71)
#define MEM_IF_MEM_ARB_CFG (MEM_IF_BASE + 0x40)
#define BATT_INFO_PEEK_MUX4 (BATT_INFO_BASE + 0xEE)
#define ALG_ACTIVE_PEEK_CFG 0xAC

#define RIF_MEM_ACCESS_REQ BIT(7)
#define GEN4_MEM_GNT_BIT BIT(3)
#define MEM_ARB_REQ_BIT BIT(0)
#define MEM_ARB_LO_LATENCY_EN_BIT BIT(1)
#define MEM_CLR_LOG_BIT BIT(2)
#define ADDR_KIND_BIT BIT(1)
#define IACS_SLCT_BIT BIT(5)

#define DMA_READ_ERROR_BIT BIT(2)
#define DMA_WRITE_ERROR_BIT BIT(1)
#define DMA_CLEAR_LOG_BIT BIT(0)

#define MEM_XCP_BIT BIT(1)

#define GEN4_FG_DMA0_BASE 0x4400
#define GEN4_FG_DMA1_BASE 0x4500
#define GEN4_FG_DMA2_BASE 0x4600
#define GEN4_FG_DMA3_BASE 0x4700
#define GEN4_FG_DMA4_BASE 0x4800
#define GEN4_FG_DMA5_BASE 0x4900
#define SRAM_ADDR_OFFSET 0x20
#define FG_GEN4_NUM_PARTITIONS 6
#define FG_GEN4_BYTES_PER_WORD 2

#define PROFILE_LOAD_WORD 65
#define PROFILE_INTEGRITY_WORD 299
#define ESR_CAL_SOC_MIN_WORD 10
#define ESR_CAL_THRESH_WORD 11
#define ESR_PULSE_THRESH_WORD 12
#define ESR_TIMER_DISCHG_WORD 17
#define ESR_TIMER_CHG_WORD 18
#define CUTOFF_CURR_WORD 19
#define CUTOFF_VOLT_WORD 20
#define SYS_TERM_CURR_WORD 22
#define KI_COEFF_LOW_CHG_WORD 28
#define KI_COEFF_LOW_CHG_OFFSET 0
#define KI_COEFF_MED_CHG_WORD 28
#define KI_COEFF_MED_CHG_OFFSET 1
#define KI_COEFF_HI_CHG_WORD 29
#define KI_COEFF_HI_CHG_OFFSET 0
#define KI_COEFF_LO_MED_CHG_THR_WORD 29
#define KI_COEFF_LO_MED_CHG_THR_OFFSET 1
#define KI_COEFF_MED_HI_CHG_THR_WORD 30
#define KI_COEFF_MED_HI_CHG_THR_OFFSET 0
#define VBATT_LOW_WORD 35
#define NOM_CAP_WORD 271
#define ACT_BATT_CAP_WORD 287
#define CYCLE_COUNT_WORD 291
#define BATT_SOC_WORD 455
#define CC_SOC_SW_WORD 464
#define BATT_TEMP_WORD 328

#define SDAM_COOKIE_OFFSET 0x80
#define SDAM_CYCLE_COUNT_OFFSET 0x81
#define SDAM_CAP_LEARN_OFFSET 0x91
#define SDAM_CYCLE_VALID_OFFSET 0x93
#define SDAM_COOKIE 0xa5
#define SDAM_CYCLE_VALID 0xc35a

#define FG_CYCLE_BUCKET_COUNT 8
#define FG_CYCLE_BUCKET_SOC_RAW (256 / FG_CYCLE_BUCKET_COUNT)

#define PROFILE_LOAD_BIT BIT(0)
#define HLOS_RESTART_BIT BIT(3)
#define PROFILE_LEN 416

#define BATT_TEMP_JEITA_COLD 100
#define BATT_TEMP_JEITA_HOT 450
#define DEFAULT_CL_MIN_TEMP_DECIDEGC 150
#define DEFAULT_CL_MAX_TEMP_DECIDEGC 500

#define CC_SOC_30BIT 0x3fffffff
#define CENTI_FULL_SOC 10000
#define BATT_SOC_32BIT 0xffffffff

/* Nominal voltage floor (μV) for ENERGY_* when DT lacks an explicit value. */
#define QCOM_FG_NOMINAL_VOLTAGE_FLOOR_UV 3700000

struct fg_dma_partition {
	u16 partition_start;
	u16 partition_end;
	u16 spmi_addr_base;
};

static const struct fg_dma_partition fg_gen4_partitions[] = {
	{ 0, 63, GEN4_FG_DMA0_BASE + SRAM_ADDR_OFFSET }, /* system */
	{ 64, 169, GEN4_FG_DMA1_BASE + SRAM_ADDR_OFFSET }, /* battery profile */
	{ 170, 274,
	  GEN4_FG_DMA2_BASE + SRAM_ADDR_OFFSET }, /* battery profile cont. */
	{ 275, 299, GEN4_FG_DMA3_BASE + SRAM_ADDR_OFFSET }, /* dp/SW */
	{ 300, 405, GEN4_FG_DMA4_BASE + SRAM_ADDR_OFFSET }, /* wk/scratch */
	{ 406, 486,
	  GEN4_FG_DMA5_BASE + SRAM_ADDR_OFFSET }, /* wk/scratch cont. */
};

struct qcom_fg_chip {
	struct device *dev;
	unsigned int base;
	struct regmap *regmap;
	struct nvmem_device *nvmem;

	struct power_supply_battery_info *batt_info;

	/* Battery identification (set once during init_capacity_learning) */
	const char *manufacturer;
	const char *model_name;
	int nom_voltage_uv;

	/* Capacity learning */
	struct mutex cl_lock;
	bool cl_active;
	int cl_init_cc_soc_sw;
	s64 nom_cap_uah;
	s64 init_cap_uah;
	s64 final_cap_uah;
	int cl_max_start_soc;
	int cl_max_cap_inc;
	int cl_max_cap_dec;
	int cl_min_temp;
	int cl_max_temp;

	/* Cycle count is accumulated in eight 12.5%-SOC buckets. */
	struct mutex cycle_lock;
	u16 cycle_count[FG_CYCLE_BUCKET_COUNT];
	bool cycle_started[FG_CYCLE_BUCKET_COUNT];
	u8 cycle_last_soc[FG_CYCLE_BUCKET_COUNT];
	int cycle_last_bucket;

	struct completion mem_attn_done;
	struct mutex dma_lock;

	/* Lock order: cl_lock -> state_lock. dma_lock is independent. */
	struct mutex state_lock;
	int status;
	bool charge_done;
	bool input_present;
	int batt_soc;
	s64 learned_cap_uah;
};

static int qcom_fg_read(struct qcom_fg_chip *chip, u8 *val, u16 addr, int len)
{
	if (((chip->base + addr) & 0xff00) == 0)
		return -EINVAL;

	return regmap_bulk_read(chip->regmap, chip->base + addr, val, len);
}

static int qcom_fg_write(struct qcom_fg_chip *chip, u8 *val, u16 addr, int len)
{
	bool sec_access = (addr & 0xff) > 0xd0;
	u8 sec_addr_val = 0xa5;
	int ret;

	if (((chip->base + addr) & 0xff00) == 0)
		return -EINVAL;

	if (sec_access) {
		ret = regmap_bulk_write(chip->regmap,
					((chip->base + addr) & 0xff00) | 0xd0,
					&sec_addr_val, 1);
		if (ret)
			return ret;
	}

	return regmap_bulk_write(chip->regmap, chip->base + addr, val, len);
}

static int qcom_fg_masked_write(struct qcom_fg_chip *chip, u16 addr, u8 mask,
				u8 val)
{
	u8 reg;
	int ret;

	ret = qcom_fg_read(chip, &reg, addr, 1);
	if (ret)
		return ret;

	reg &= ~mask;
	reg |= val & mask;

	return qcom_fg_write(chip, &reg, addr, 1);
}

/* Gen4 FG SRAM spans 6 DMA partitions; cross-partition transfers are handled here. */

/* Resolve (sram_addr, offset) to SPMI addr + bytes available in partition. */
static int qcom_fg_dma_resolve(struct qcom_fg_chip *chip, u16 sram_addr,
			       u8 offset, int len, u16 *addr, int *avail)
{
	int i;

	for (i = 0; i < FG_GEN4_NUM_PARTITIONS; i++) {
		if (sram_addr >= fg_gen4_partitions[i].partition_start &&
		    sram_addr <= fg_gen4_partitions[i].partition_end) {
			*addr = fg_gen4_partitions[i].spmi_addr_base + offset +
				(sram_addr -
				 fg_gen4_partitions[i].partition_start) *
					FG_GEN4_BYTES_PER_WORD;
			*avail = (fg_gen4_partitions[i].partition_end -
				  sram_addr + 1) *
					 FG_GEN4_BYTES_PER_WORD -
				 offset;
			if (*avail > len)
				*avail = len;
			return 0;
		}
	}

	dev_err(chip->dev, "Couldn't find DMA partition for SRAM word %d\n",
		sram_addr);
	return -ENXIO;
}

static irqreturn_t qcom_fg_mem_attn_irq_handler(int irq, void *data)
{
	struct qcom_fg_chip *chip = data;

	complete_all(&chip->mem_attn_done);
	return IRQ_HANDLED;
}

static int qcom_fg_clear_dma_errors(struct qcom_fg_chip *chip)
{
	u8 dma_sts;
	bool error_present;
	int ret;

	ret = qcom_fg_read(chip, &dma_sts, MEM_IF_DMA_STS, 1);
	if (ret)
		return ret;

	error_present = dma_sts & (DMA_WRITE_ERROR_BIT | DMA_READ_ERROR_BIT);
	return qcom_fg_masked_write(chip, MEM_IF_DMA_CTL, DMA_CLEAR_LOG_BIT,
				    error_present ? DMA_CLEAR_LOG_BIT : 0);
}

static irqreturn_t qcom_fg_mem_xcp_irq_handler(int irq, void *data)
{
	struct qcom_fg_chip *chip = data;
	u8 status;
	int ret;

	ret = qcom_fg_read(chip, &status, MEM_IF_INT_RT_STS, 1);
	if (ret < 0) {
		dev_err(chip->dev, "failed to read MEM_IF_INT_RT_STS: %d\n",
			ret);
		return IRQ_HANDLED;
	}

	mutex_lock(&chip->dma_lock);
	ret = qcom_fg_clear_dma_errors(chip);
	mutex_unlock(&chip->dma_lock);
	if (ret < 0)
		dev_err(chip->dev, "Error in clearing DMA error: %d\n", ret);

	if (status & MEM_XCP_BIT)
		dev_err(chip->dev, "MEM_XCP asserted, status=0x%02x\n", status);

	return IRQ_HANDLED;
}

static int qcom_fg_dma_request(struct qcom_fg_chip *chip)
{
	u8 sts;
	int ret;

	/* Arm the completion before asserting the request to avoid losing IRQs. */
	reinit_completion(&chip->mem_attn_done);

	ret = qcom_fg_masked_write(chip, MEM_IF_MEM_ARB_CFG, MEM_ARB_REQ_BIT,
				   MEM_ARB_REQ_BIT);
	if (ret)
		return ret;

	ret = qcom_fg_masked_write(chip, MEM_INTF_CFG,
				   RIF_MEM_ACCESS_REQ | IACS_SLCT_BIT,
				   RIF_MEM_ACCESS_REQ);
	if (ret)
		goto out_release_arb;

	/* Skip wait if MEM_GNT already set. */
	ret = qcom_fg_read(chip, &sts, MEM_IF_INT_RT_STS, 1);
	if (ret)
		goto out_release;

	if (sts & GEN4_MEM_GNT_BIT)
		return 0;

	if (!wait_for_completion_timeout(&chip->mem_attn_done,
					 msecs_to_jiffies(1000))) {
		ret = -ETIMEDOUT;
		goto out_release;
	}

	ret = qcom_fg_read(chip, &sts, MEM_IF_INT_RT_STS, 1);
	if (ret)
		goto out_release;

	if (sts & GEN4_MEM_GNT_BIT)
		return 0;

	ret = -ETIMEDOUT;
out_release:
	qcom_fg_masked_write(chip, MEM_INTF_CFG,
			     RIF_MEM_ACCESS_REQ | IACS_SLCT_BIT, 0);
out_release_arb:
	qcom_fg_masked_write(chip, MEM_IF_MEM_ARB_CFG, MEM_ARB_REQ_BIT, 0);
	return ret;
}

static int qcom_fg_dma_release(struct qcom_fg_chip *chip)
{
	int arb_ret;
	int ret;

	ret = qcom_fg_masked_write(chip, MEM_INTF_CFG,
				   RIF_MEM_ACCESS_REQ | IACS_SLCT_BIT, 0);
	arb_ret = qcom_fg_masked_write(chip, MEM_IF_MEM_ARB_CFG,
				       MEM_ARB_REQ_BIT, 0);

	return ret ?: arb_ret;
}

/* @offset < FG_GEN4_BYTES_PER_WORD. */
static int qcom_fg_sram_xfer(struct qcom_fg_chip *chip, u16 sram_addr,
			     u8 offset, u8 *val, int len, bool write)
{
	u8 *ptr = val;
	u16 addr;
	int num_bytes, ret;

	if (WARN_ON_ONCE(offset >= FG_GEN4_BYTES_PER_WORD || len <= 0))
		return -EINVAL;

	mutex_lock(&chip->dma_lock);

	ret = qcom_fg_dma_request(chip);
	if (ret) {
		dev_err(chip->dev, "Failed to request DMA access: %d\n", ret);
		mutex_unlock(&chip->dma_lock);
		return ret;
	}

	while (len > 0) {
		ret = qcom_fg_dma_resolve(chip, sram_addr, offset, len, &addr,
					  &num_bytes);
		if (ret)
			goto out;

		if (write)
			ret = regmap_bulk_write(chip->regmap, addr, ptr,
						num_bytes);
		else
			ret = regmap_bulk_read(chip->regmap, addr, ptr,
					       num_bytes);
		if (ret) {
			dev_err(chip->dev, "DMA %s failed at word %d: %d\n",
				write ? "write" : "read", sram_addr, ret);
			goto out;
		}

		ptr += num_bytes;
		len -= num_bytes;
		sram_addr += num_bytes / FG_GEN4_BYTES_PER_WORD;
		offset = 0;
	}

out: {
	int release_ret = qcom_fg_dma_release(chip);

	if (!ret)
		ret = release_ret;
}
	mutex_unlock(&chip->dma_lock);
	return ret;
}

static int qcom_fg_sram_write(struct qcom_fg_chip *chip, u16 sram_addr,
			      u8 offset, u8 *val, int len)
{
	return qcom_fg_sram_xfer(chip, sram_addr, offset, val, len, true);
}

static int qcom_fg_sram_read(struct qcom_fg_chip *chip, u16 sram_addr,
			     u8 offset, u8 *val, int len)
{
	return qcom_fg_sram_xfer(chip, sram_addr, offset, val, len, false);
}

static int qcom_fg_dma_init(struct qcom_fg_chip *chip)
{
	int ret;
	u8 val;

	/* Configure the DMA peripheral addressing to partition mode */
	ret = qcom_fg_masked_write(chip, MEM_IF_DMA_CTL, ADDR_KIND_BIT,
				   ADDR_KIND_BIT);
	if (ret) {
		dev_err(chip->dev, "Failed to configure DMA_CTL: %d\n", ret);
		return ret;
	}

	/* Release DMA so that request can happen. */
	ret = qcom_fg_dma_release(chip);
	if (ret) {
		dev_err(chip->dev, "Failed to release DMA access: %d\n", ret);
		return ret;
	}

	/* Set low latency and clear log bit */
	ret = qcom_fg_masked_write(chip, MEM_IF_MEM_ARB_CFG,
				   MEM_ARB_LO_LATENCY_EN_BIT | MEM_CLR_LOG_BIT,
				   MEM_ARB_LO_LATENCY_EN_BIT);
	if (ret) {
		dev_err(chip->dev, "Failed to configure MEM_ARB_CFG: %d\n",
			ret);
		return ret;
	}

	/* Configure PEEK_MUX4 for ALG active signal */
	val = ALG_ACTIVE_PEEK_CFG;
	ret = qcom_fg_write(chip, &val, BATT_INFO_PEEK_MUX4, 1);
	if (ret) {
		dev_err(chip->dev, "Failed to configure PEEK_MUX4: %d\n", ret);
		return ret;
	}

	return 0;
}

static int qcom_fg_get_cc_soc_sw(struct qcom_fg_chip *chip, int *cc_soc_sw)
{
	u8 buf[4];
	int ret;

	ret = qcom_fg_sram_read(chip, CC_SOC_SW_WORD, 0, buf, 4);
	if (ret)
		return ret;

	*cc_soc_sw = get_unaligned_le32(buf) & CC_SOC_30BIT;
	return 0;
}

static int qcom_fg_prime_cc_soc_sw(struct qcom_fg_chip *chip, u32 cc_soc_sw)
{
	u8 buf[4];

	put_unaligned_le32(cc_soc_sw & CC_SOC_30BIT, buf);

	return qcom_fg_sram_write(chip, CC_SOC_SW_WORD, 0, buf, 4);
}

static int qcom_fg_get_batt_soc(struct qcom_fg_chip *chip, int *batt_soc_cp,
				u8 *batt_soc_raw)
{
	u8 buf[4];
	u32 batt_soc;
	int ret;

	ret = qcom_fg_sram_read(chip, BATT_SOC_WORD, 0, buf, sizeof(buf));
	if (ret)
		return ret;

	batt_soc = get_unaligned_le32(buf);
	if (batt_soc_cp)
		*batt_soc_cp = div64_u64((u64)batt_soc * CENTI_FULL_SOC,
					 BATT_SOC_32BIT);
	if (batt_soc_raw)
		*batt_soc_raw = batt_soc >> 24;

	return 0;
}

static int qcom_fg_read_sram_learned_capacity(struct qcom_fg_chip *chip,
					      s64 *cap_uah)
{
	u8 buf[2];
	int ret;

	ret = qcom_fg_sram_read(chip, ACT_BATT_CAP_WORD, 0, buf, sizeof(buf));
	if (ret)
		return ret;

	*cap_uah = (s64)get_unaligned_le16(buf) * 1000;
	return *cap_uah > 0 ? 0 : -ENODATA;
}

static int qcom_fg_write_sram_learned_capacity(struct qcom_fg_chip *chip,
					       u8 *buf)
{
	return qcom_fg_sram_write(chip, ACT_BATT_CAP_WORD, 0, buf, 2);
}

static int qcom_fg_read_sdam_learned_capacity(struct qcom_fg_chip *chip,
					      s64 *cap_uah)
{
	u8 buf[2], cookie;
	int ret;

	ret = nvmem_device_read(chip->nvmem, SDAM_COOKIE_OFFSET, 1, &cookie);
	if (ret < 0)
		return ret;
	if (ret != 1)
		return -EIO;
	if (cookie != SDAM_COOKIE)
		return -ENODATA;

	ret = nvmem_device_read(chip->nvmem, SDAM_CAP_LEARN_OFFSET, sizeof(buf),
				buf);
	if (ret < 0)
		return ret;
	if (ret != sizeof(buf))
		return -EIO;

	*cap_uah = (s64)get_unaligned_le16(buf) * 1000;
	return *cap_uah > 0 ? 0 : -ENODATA;
}

static int qcom_fg_write_sdam_learned_capacity(struct qcom_fg_chip *chip,
					       u8 *buf)
{
	u8 cookie = SDAM_COOKIE;
	int ret;

	ret = nvmem_device_write(chip->nvmem, SDAM_CAP_LEARN_OFFSET, 2, buf);
	if (ret < 0)
		return ret;
	if (ret != 2)
		return -EIO;

	ret = nvmem_device_write(chip->nvmem, SDAM_COOKIE_OFFSET, 1, &cookie);
	if (ret < 0)
		return ret;

	return ret == 1 ? 0 : -EIO;
}

static int qcom_fg_get_learned_capacity(struct qcom_fg_chip *chip, s64 *cap_uah)
{
	u8 buf[2];
	int ret;

	if (!chip->nvmem)
		return qcom_fg_read_sram_learned_capacity(chip, cap_uah);

	ret = qcom_fg_read_sdam_learned_capacity(chip, cap_uah);
	if (!ret) {
		put_unaligned_le16(div_s64(*cap_uah, 1000), buf);
		ret = qcom_fg_write_sram_learned_capacity(chip, buf);
		if (ret)
			dev_warn(
				chip->dev,
				"Failed to restore learned capacity to SRAM: %d\n",
				ret);
		return 0;
	}

	if (ret != -ENODATA)
		dev_warn(chip->dev,
			 "Failed to read learned capacity from SDAM: %d\n",
			 ret);

	ret = qcom_fg_read_sram_learned_capacity(chip, cap_uah);
	if (ret)
		return ret;

	put_unaligned_le16(div_s64(*cap_uah, 1000), buf);
	ret = qcom_fg_write_sdam_learned_capacity(chip, buf);
	if (ret)
		dev_warn(chip->dev,
			 "Failed to seed learned capacity in SDAM: %d\n", ret);

	return 0;
}

static int qcom_fg_store_learned_capacity(struct qcom_fg_chip *chip,
					  s64 learned_cap_uah)
{
	u8 buf[2];
	int cc_mah, ret;

	if (learned_cap_uah <= 0 || learned_cap_uah > (s64)U16_MAX * 1000)
		return -ERANGE;

	cc_mah = (int)div_s64(learned_cap_uah, 1000);
	put_unaligned_le16(cc_mah, buf);

	ret = qcom_fg_write_sram_learned_capacity(chip, buf);
	if (ret || !chip->nvmem)
		return ret;

	return qcom_fg_write_sdam_learned_capacity(chip, buf);
}

/*
 * The Gen4 FG tracks charge throughput in eight SOC buckets.  SDAM retains
 * the bucket counters across hard resets; SRAM is kept in sync for firmware
 * compatibility and as a fallback when SDAM is unavailable.
 */
static int qcom_fg_write_cycle_marker(struct qcom_fg_chip *chip)
{
	u8 marker[2];
	int ret;

	put_unaligned_le16(SDAM_CYCLE_VALID, marker);
	ret = nvmem_device_write(chip->nvmem, SDAM_CYCLE_VALID_OFFSET,
				 sizeof(marker), marker);
	if (ret < 0)
		return ret;

	return ret == sizeof(marker) ? 0 : -EIO;
}

static int qcom_fg_write_sdam_cycle_counts(struct qcom_fg_chip *chip, u8 *buf,
					   size_t len, unsigned int offset)
{
	int ret;

	ret = nvmem_device_write(chip->nvmem, SDAM_CYCLE_COUNT_OFFSET + offset,
				 len, buf);
	if (ret < 0)
		return ret;

	return ret == len ? 0 : -EIO;
}

static int qcom_fg_restore_cycle_count(struct qcom_fg_chip *chip)
{
	u8 buf[FG_CYCLE_BUCKET_COUNT * sizeof(u16)];
	u8 marker[2] = {};
	bool restore_sdam = false;
	int i, ret;

	if (chip->nvmem) {
		ret = nvmem_device_read(chip->nvmem, SDAM_CYCLE_VALID_OFFSET,
					sizeof(marker), marker);
		if (ret == sizeof(marker) &&
		    get_unaligned_le16(marker) == SDAM_CYCLE_VALID) {
			ret = nvmem_device_read(chip->nvmem,
						SDAM_CYCLE_COUNT_OFFSET,
						sizeof(buf), buf);
			if (ret == sizeof(buf))
				restore_sdam = true;
			else
				dev_warn(
					chip->dev,
					"Failed to restore cycle count from SDAM: %d\n",
					ret < 0 ? ret : -EIO);
		} else if (ret < 0) {
			dev_warn(chip->dev,
				 "Failed to read cycle-count SDAM marker: %d\n",
				 ret);
		}
	}

	if (!restore_sdam) {
		ret = qcom_fg_sram_read(chip, CYCLE_COUNT_WORD, 0, buf,
					sizeof(buf));
		if (ret)
			return ret;

		if (chip->nvmem) {
			ret = qcom_fg_write_sdam_cycle_counts(chip, buf,
							      sizeof(buf), 0);
			if (!ret)
				ret = qcom_fg_write_cycle_marker(chip);
			if (ret)
				dev_warn(
					chip->dev,
					"Failed to seed cycle count in SDAM: %d\n",
					ret);
		}
	} else {
		ret = qcom_fg_sram_write(chip, CYCLE_COUNT_WORD, 0, buf,
					 sizeof(buf));
		if (ret)
			dev_warn(chip->dev,
				 "Failed to restore cycle count to SRAM: %d\n",
				 ret);
	}

	mutex_lock(&chip->cycle_lock);
	for (i = 0; i < FG_CYCLE_BUCKET_COUNT; i++)
		chip->cycle_count[i] = get_unaligned_le16(buf + i * 2);
	mutex_unlock(&chip->cycle_lock);

	return 0;
}

static int qcom_fg_store_cycle_bucket(struct qcom_fg_chip *chip, int id,
				      u16 count)
{
	u8 buf[2];
	int ret;

	put_unaligned_le16(count, buf);

	if (chip->nvmem) {
		ret = qcom_fg_write_sdam_cycle_counts(chip, buf, sizeof(buf),
						      id * sizeof(u16));
		if (ret)
			return ret;

		ret = qcom_fg_sram_write(chip, CYCLE_COUNT_WORD + id, 0, buf,
					 sizeof(buf));
		if (ret)
			dev_warn(
				chip->dev,
				"Failed to mirror cycle bucket %d to SRAM: %d\n",
				id, ret);

		return 0;
	}

	return qcom_fg_sram_write(chip, CYCLE_COUNT_WORD + id, 0, buf,
				  sizeof(buf));
}

static void qcom_fg_cycle_count_update(struct qcom_fg_chip *chip, u8 batt_soc,
				       int status, bool charge_done,
				       bool input_present)
{
	int id = batt_soc / FG_CYCLE_BUCKET_SOC_RAW;
	int i, ret;
	u16 count;

	mutex_lock(&chip->cycle_lock);

	if (status == POWER_SUPPLY_STATUS_CHARGING) {
		if (!chip->cycle_started[id] && id != chip->cycle_last_bucket) {
			chip->cycle_started[id] = true;
			chip->cycle_last_soc[id] = batt_soc;
		}
	} else if (charge_done || !input_present) {
		for (i = 0; i < FG_CYCLE_BUCKET_COUNT; i++) {
			if (!chip->cycle_started[i] ||
			    batt_soc <= chip->cycle_last_soc[i] +
						FG_CYCLE_BUCKET_SOC_RAW / 2)
				continue;

			if (chip->cycle_count[i] == U16_MAX) {
				chip->cycle_started[i] = false;
				chip->cycle_last_soc[i] = 0;
				chip->cycle_last_bucket = i;
				continue;
			}

			count = chip->cycle_count[i] + 1;
			ret = qcom_fg_store_cycle_bucket(chip, i, count);
			if (ret) {
				dev_warn(
					chip->dev,
					"Failed to store cycle bucket %d: %d\n",
					i, ret);
				continue;
			}

			chip->cycle_count[i] = count;
			chip->cycle_started[i] = false;
			chip->cycle_last_soc[i] = 0;
			chip->cycle_last_bucket = i;
			dev_dbg(chip->dev, "Cycle bucket %d count=%u\n", i,
				count);
		}
	}

	mutex_unlock(&chip->cycle_lock);
}

static int qcom_fg_get_cycle_count(struct qcom_fg_chip *chip, int *count)
{
	u32 sum = 0;
	int i;

	mutex_lock(&chip->cycle_lock);
	for (i = 0; i < FG_CYCLE_BUCKET_COUNT; i++)
		sum += chip->cycle_count[i];
	mutex_unlock(&chip->cycle_lock);

	*count = sum / FG_CYCLE_BUCKET_COUNT;
	return 0;
}

static int qcom_fg_cap_learning_begin(struct qcom_fg_chip *chip,
				      int batt_soc_cp)
{
	int batt_soc_pct, cc_soc_sw, ret;
	s64 learned_cap;
	u32 batt_soc_prime;

	batt_soc_pct = DIV_ROUND_CLOSEST(batt_soc_cp, 100);

	if (chip->cl_max_start_soc > 0 && batt_soc_pct > chip->cl_max_start_soc)
		return -EINVAL;

	mutex_lock(&chip->state_lock);
	learned_cap = chip->learned_cap_uah;
	mutex_unlock(&chip->state_lock);

	chip->init_cap_uah =
		div64_s64(learned_cap * batt_soc_cp, CENTI_FULL_SOC);

	batt_soc_prime =
		div64_u64((u64)batt_soc_cp * CC_SOC_30BIT, CENTI_FULL_SOC);
	ret = qcom_fg_prime_cc_soc_sw(chip, batt_soc_prime);
	if (ret)
		return ret;

	ret = qcom_fg_get_cc_soc_sw(chip, &cc_soc_sw);
	if (ret)
		return ret;

	chip->cl_init_cc_soc_sw = cc_soc_sw;
	chip->cl_active = true;

	dev_dbg(chip->dev,
		"Cap learning started: soc=%d%%, init_cap=%lld uAh\n",
		batt_soc_pct, chip->init_cap_uah);
	return 0;
}

static s64 qcom_fg_cap_learning_post_process(struct qcom_fg_chip *chip)
{
	s64 max_inc_val, min_dec_val;

	max_inc_val = div64_s64(
		chip->learned_cap_uah * (1000 + chip->cl_max_cap_inc), 1000);
	min_dec_val = div64_s64(
		chip->learned_cap_uah * (1000 - chip->cl_max_cap_dec), 1000);

	return clamp_t(s64, chip->final_cap_uah, min_dec_val, max_inc_val);
}

static int qcom_fg_cap_learning_done(struct qcom_fg_chip *chip)
{
	int cc_soc_sw, cc_soc_delta_pct, cc_soc_fraction;
	s64 cc_soc_cap_uah, cc_soc_fraction_uah, delta_cap_uah, new_cap_uah;
	int ret;

	ret = qcom_fg_get_cc_soc_sw(chip, &cc_soc_sw);
	if (ret)
		goto out;

	cc_soc_delta_pct =
		div_s64_rem((s64)(cc_soc_sw - chip->cl_init_cc_soc_sw) * 100,
			    CC_SOC_30BIT, &cc_soc_fraction);

	if (cc_soc_delta_pct < 50) {
		if (cc_soc_delta_pct < 0)
			dev_warn(
				chip->dev,
				"Cap learning: negative delta %d%%, skipping\n",
				cc_soc_delta_pct);
		else
			dev_dbg(chip->dev,
				"Cap learning: delta %d%% too small, skipping\n",
				cc_soc_delta_pct);
		goto out;
	}

	cc_soc_cap_uah =
		div64_s64(chip->learned_cap_uah * cc_soc_delta_pct, 100);
	cc_soc_fraction_uah = div64_s64(chip->learned_cap_uah * cc_soc_fraction,
					(s64)CC_SOC_30BIT * 100);
	delta_cap_uah = cc_soc_cap_uah + cc_soc_fraction_uah;

	chip->final_cap_uah = chip->init_cap_uah + delta_cap_uah;
	new_cap_uah = qcom_fg_cap_learning_post_process(chip);

	ret = qcom_fg_store_learned_capacity(chip, new_cap_uah);
	if (ret)
		goto out;

	ret = qcom_fg_prime_cc_soc_sw(chip, CC_SOC_30BIT);
	if (ret)
		goto out;

	mutex_lock(&chip->state_lock);
	chip->learned_cap_uah = new_cap_uah;
	mutex_unlock(&chip->state_lock);

	dev_info(chip->dev,
		 "Cap learning done: new_cap=%lld uAh (delta=%lld uAh)\n",
		 new_cap_uah, chip->final_cap_uah - chip->init_cap_uah);

out:
	chip->cl_active = false;
	return ret;
}

static void qcom_fg_cap_learning_abort(struct qcom_fg_chip *chip,
				       int batt_soc_cp, const char *reason)
{
	u32 batt_soc_prime;
	int ret;

	chip->cl_active = false;
	batt_soc_prime =
		div64_u64((u64)batt_soc_cp * CC_SOC_30BIT, CENTI_FULL_SOC);
	ret = qcom_fg_prime_cc_soc_sw(chip, batt_soc_prime);
	if (ret)
		dev_warn(
			chip->dev,
			"Failed to reset CC_SOC after capacity-learning abort: %d\n",
			ret);

	dev_dbg(chip->dev, "Cap learning aborted: %s\n", reason);
}

static void qcom_fg_cap_learning_update(struct qcom_fg_chip *chip,
					int batt_temp, int batt_soc_cp,
					int status, bool charge_done,
					bool input_present)
{
	bool has_cap;

	mutex_lock(&chip->state_lock);
	has_cap = chip->learned_cap_uah > 0;
	mutex_unlock(&chip->state_lock);
	if (!has_cap)
		return;

	mutex_lock(&chip->cl_lock);

	if (batt_temp < chip->cl_min_temp || batt_temp > chip->cl_max_temp) {
		if (chip->cl_active) {
			qcom_fg_cap_learning_abort(chip, batt_soc_cp,
						   "temperature out of range");
		}
		goto unlock;
	}

	if (chip->cl_active) {
		if (charge_done) {
			if (qcom_fg_cap_learning_done(chip))
				dev_warn(chip->dev,
					 "Cap learning finalization failed\n");
		} else if (status == POWER_SUPPLY_STATUS_NOT_CHARGING) {
			qcom_fg_cap_learning_abort(chip, batt_soc_cp,
						   "charging stopped");
		} else if (status == POWER_SUPPLY_STATUS_DISCHARGING &&
			   !input_present) {
			qcom_fg_cap_learning_abort(chip, batt_soc_cp,
						   "input removed");
		}
	} else if (status == POWER_SUPPLY_STATUS_CHARGING && input_present) {
		if (qcom_fg_cap_learning_begin(chip, batt_soc_cp))
			dev_dbg(chip->dev, "Cap learning begin failed\n");
	}

unlock:
	mutex_unlock(&chip->cl_lock);
}

#define FG_MAX_READ_TRIES 5

static int qcom_fg_get_capacity(struct qcom_fg_chip *chip, int *val)
{
	bool report_full;
	u8 cap[2];
	int ret, tries = 0;

	/*
	 * MONOTONIC_SOC has shadow registers at offset 0x09 and 0x0A. Retry
	 * until they match; do not fall back to min() which can under-report.
	 */
	while (tries < FG_MAX_READ_TRIES) {
		ret = qcom_fg_read(chip, cap, BATT_MONOTONIC_SOC, 2);
		if (ret) {
			dev_err(chip->dev, "Failed to read capacity: %d\n",
				ret);
			return ret;
		}

		if (cap[0] == cap[1])
			break;

		tries++;
	}

	if (tries == FG_MAX_READ_TRIES) {
		dev_err(chip->dev, "MSOC: shadow registers do not match\n");
		return -EINVAL;
	}

	mutex_lock(&chip->state_lock);
	report_full = (chip->status == POWER_SUPPLY_STATUS_FULL);
	mutex_unlock(&chip->state_lock);

	if (cap[0] == 0) {
		*val = 0;
	} else if (cap[0] == FULL_SOC_RAW) {
		*val = 100;
	} else if (report_full && cap[0] >= FULL_SOC_REPORT_THR - 2) {
		int pct = DIV_ROUND_CLOSEST(cap[0] * 100, FULL_SOC_RAW);

		*val = min(100, pct + 1);
	} else if (report_full && cap[0] >= FULL_SOC_REPORT_THR - 4) {
		*val = DIV_ROUND_CLOSEST(cap[0] * 100, FULL_SOC_RAW);
	} else {
		*val = DIV_ROUND_CLOSEST((cap[0] - 1) * 98, FULL_SOC_RAW - 2) +
		       1;
	}

	return 0;
}

static int qcom_fg_get_temperature(struct qcom_fg_chip *chip, int *val)
{
	u8 readval[2];
	int ret;

	ret = qcom_fg_sram_read(chip, BATT_TEMP_WORD, 0, readval, 2);
	if (ret) {
		dev_err(chip->dev, "Failed to read battery temperature: %d\n",
			ret);
		return ret;
	}

	*val = sign_extend32(get_unaligned_le16(readval), 9) * 100 / 40;
	return 0;
}

static void qcom_fg_algorithms_update(struct qcom_fg_chip *chip)
{
	bool charge_done, input_present;
	u8 batt_soc_raw;
	int batt_soc_cp, batt_temp, status;
	int ret;

	mutex_lock(&chip->state_lock);
	status = chip->status;
	charge_done = chip->charge_done;
	input_present = chip->input_present;
	mutex_unlock(&chip->state_lock);

	ret = qcom_fg_get_batt_soc(chip, &batt_soc_cp, &batt_soc_raw);
	if (ret) {
		dev_warn(chip->dev,
			 "Failed to read SOC for FG algorithms: %d\n", ret);
		return;
	}

	qcom_fg_cycle_count_update(chip, batt_soc_raw, status, charge_done,
				   input_present);

	ret = qcom_fg_get_temperature(chip, &batt_temp);
	if (ret)
		return;

	qcom_fg_cap_learning_update(chip, batt_temp, batt_soc_cp, status,
				    charge_done, input_present);
}

/*
 * IBATT/VBATT are each backed by a shadow (checkpoint) register that must agree
 * with the primary before the value is consumed.  Retry a few times and fail if
 * they never settle.  On success @buf holds the matching primary data.
 */
static int qcom_fg_read_shadow_pair(struct qcom_fg_chip *chip, u16 addr,
				    u16 addr_cp, u8 buf[2], const char *name)
{
	u8 buf_cp[2];
	int ret, tries = 0;

	while (tries < FG_MAX_READ_TRIES) {
		ret = qcom_fg_read(chip, buf, addr, 2);
		if (ret) {
			dev_err(chip->dev, "Failed to read %s: %d\n", name, ret);
			return ret;
		}

		ret = qcom_fg_read(chip, buf_cp, addr_cp, 2);
		if (ret) {
			dev_err(chip->dev, "Failed to read %s CP: %d\n", name,
				ret);
			return ret;
		}

		if (buf[0] == buf_cp[0] && buf[1] == buf_cp[1])
			return 0;

		tries++;
	}

	dev_err(chip->dev, "%s: shadow registers do not match\n", name);
	return -EINVAL;
}

static int qcom_fg_get_current(struct qcom_fg_chip *chip, int *val)
{
	s16 temp;
	u8 buf[2];
	int ret;

	/* IBATT shadow registers at 0xA2 and 0xA8; retry until they match. */
	ret = qcom_fg_read_shadow_pair(chip, PARAM_ADDR_BATT_CURRENT,
					PARAM_ADDR_BATT_CURRENT_CP, buf, "IBATT");
	if (ret)
		return ret;

	temp = (s16)get_unaligned_le16(buf);

	/* FG: discharge-positive.  Invert to power_supply (charge-positive). */
	*val = -div_s64((s64)temp * BATT_CURRENT_NUMR, BATT_CURRENT_DENR);

	return 0;
}

static int qcom_fg_get_voltage(struct qcom_fg_chip *chip, int *val)
{
	u8 buf[2];
	int ret;

	/* VBATT shadow registers at 0xA0 and 0xA6; retry until they match. */
	ret = qcom_fg_read_shadow_pair(chip, PARAM_ADDR_BATT_VOLTAGE,
					PARAM_ADDR_BATT_VOLTAGE_CP, buf, "VBATT");
	if (ret)
		return ret;

	*val = div_u64((u64)get_unaligned_le16(buf) * BATT_VOLTAGE_NUMR,
		       BATT_VOLTAGE_DENR);

	return 0;
}

static int qcom_fg_parse_battery_info(struct qcom_fg_chip *chip)
{
	struct device *dev = chip->dev;
	struct device_node *node = dev->of_node;
	struct device_node *batt_np;
	struct power_supply_battery_info *info;
	u32 val;

	batt_np = of_parse_phandle(node, "monitored-battery", 0);
	if (!batt_np) {
		dev_err(dev, "No monitored-battery phandle\n");
		return -ENODEV;
	}

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info) {
		of_node_put(batt_np);
		return -ENOMEM;
	}

	info->voltage_min_design_uv = 3400000;
	info->voltage_max_design_uv = 4480000;
	info->charge_full_design_uah = 8720000;

	if (!of_property_read_u32(batt_np, "voltage-min-design-microvolt",
				  &val))
		info->voltage_min_design_uv = (int)val;
	if (!of_property_read_u32(batt_np, "voltage-max-design-microvolt",
				  &val))
		info->voltage_max_design_uv = (int)val;
	if (!of_property_read_u32(batt_np, "charge-full-design-microamp-hours",
				  &val))
		info->charge_full_design_uah = (int)val;

	chip->batt_info = info;
	of_node_put(batt_np);

	return 0;
}

static int qcom_fg_op_capacity(void *priv, int *val)
{
	return qcom_fg_get_capacity(priv, val);
}

static int qcom_fg_op_capacity_level(void *priv, int *val)
{
	struct qcom_fg_chip *chip = priv;
	int soc;

	mutex_lock(&chip->state_lock);
	soc = chip->batt_soc;
	mutex_unlock(&chip->state_lock);

	if (soc >= 100)
		*val = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
	else if (soc >= 90)
		*val = POWER_SUPPLY_CAPACITY_LEVEL_HIGH;
	else if (soc >= 20)
		*val = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
	else if (soc >= 5)
		*val = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
	else if (soc > 0)
		*val = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
	else
		*val = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;
	return 0;
}

static int qcom_fg_op_current(void *priv, int *val)
{
	return qcom_fg_get_current(priv, val);
}

static int qcom_fg_op_voltage(void *priv, int *val)
{
	return qcom_fg_get_voltage(priv, val);
}

static int qcom_fg_op_power(void *priv, int *val)
{
	struct qcom_fg_chip *chip = priv;
	int voltage_uv, current_ua;
	s64 power_uw;
	int ret;

	ret = qcom_fg_get_voltage(chip, &voltage_uv);
	if (ret)
		return ret;
	ret = qcom_fg_get_current(chip, &current_ua);
	if (ret)
		return ret;

	power_uw = (s64)voltage_uv * current_ua;
	*val = (int)div_s64(power_uw, 1000000);
	return 0;
}

static int qcom_fg_op_voltage_min_design(void *priv, int *val)
{
	struct qcom_fg_chip *chip = priv;

	*val = chip->batt_info->voltage_min_design_uv;
	return 0;
}

static int qcom_fg_op_voltage_max_design(void *priv, int *val)
{
	struct qcom_fg_chip *chip = priv;

	*val = chip->batt_info->voltage_max_design_uv;
	return 0;
}

static int qcom_fg_op_charge_full_design(void *priv, int *val)
{
	struct qcom_fg_chip *chip = priv;

	*val = chip->batt_info->charge_full_design_uah;
	return 0;
}

static int qcom_fg_op_charge_full(void *priv, int *val)
{
	struct qcom_fg_chip *chip = priv;

	mutex_lock(&chip->state_lock);
	*val = (int)chip->learned_cap_uah;
	mutex_unlock(&chip->state_lock);
	return 0;
}

static int qcom_fg_op_charge_now(void *priv, int *val)
{
	struct qcom_fg_chip *chip = priv;
	s64 cap;
	int soc;

	mutex_lock(&chip->state_lock);
	cap = chip->learned_cap_uah;
	soc = chip->batt_soc;
	mutex_unlock(&chip->state_lock);

	*val = (int)div_s64(cap * soc, 100);
	return 0;
}

static int qcom_fg_op_energy_full_design(void *priv, int *val)
{
	struct qcom_fg_chip *chip = priv;
	s64 nom;
	int v;

	mutex_lock(&chip->state_lock);
	nom = chip->nom_cap_uah;
	v = chip->nom_voltage_uv;
	mutex_unlock(&chip->state_lock);

	*val = (int)div_s64(nom * v, 1000000);
	return 0;
}

static int qcom_fg_op_energy_full(void *priv, int *val)
{
	struct qcom_fg_chip *chip = priv;
	s64 cap;
	int v;

	mutex_lock(&chip->state_lock);
	cap = chip->learned_cap_uah;
	v = chip->nom_voltage_uv;
	mutex_unlock(&chip->state_lock);

	*val = (int)div_s64(cap * v, 1000000);
	return 0;
}

static int qcom_fg_op_energy_now(void *priv, int *val)
{
	struct qcom_fg_chip *chip = priv;
	s64 cap;
	int soc, v;

	mutex_lock(&chip->state_lock);
	cap = chip->learned_cap_uah;
	soc = chip->batt_soc;
	v = chip->nom_voltage_uv;
	mutex_unlock(&chip->state_lock);

	*val = (int)div_s64(soc * cap * v, 100 * 1000000);
	return 0;
}

static int qcom_fg_op_cycle_count(void *priv, int *val)
{
	return qcom_fg_get_cycle_count(priv, val);
}

static int qcom_fg_op_health(void *priv, int *val)
{
	struct qcom_fg_chip *chip = priv;
	int batt_temp;
	int ret;

	ret = qcom_fg_get_temperature(chip, &batt_temp);
	if (ret)
		return ret;

	if (batt_temp < BATT_TEMP_JEITA_COLD)
		*val = POWER_SUPPLY_HEALTH_COLD;
	else if (batt_temp > BATT_TEMP_JEITA_HOT)
		*val = POWER_SUPPLY_HEALTH_OVERHEAT;
	else
		*val = POWER_SUPPLY_HEALTH_GOOD;
	return 0;
}

static int qcom_fg_op_present(void *priv, int *val)
{
	*val = 1;
	return 0;
}

static int qcom_fg_op_temp(void *priv, int *val)
{
	return qcom_fg_get_temperature(priv, val);
}

static int qcom_fg_op_scope(void *priv, int *val)
{
	*val = POWER_SUPPLY_SCOPE_SYSTEM;
	return 0;
}

static int qcom_fg_op_technology(void *priv, int *val)
{
	*val = POWER_SUPPLY_TECHNOLOGY_LION;
	return 0;
}

static int qcom_fg_op_manufacturer(void *priv, const char **name)
{
	struct qcom_fg_chip *chip = priv;

	*name = chip->manufacturer;
	return 0;
}

static int qcom_fg_op_model_name(void *priv, const char **name)
{
	struct qcom_fg_chip *chip = priv;

	*name = chip->model_name;
	return 0;
}

static int qcom_fg_op_set_charge_full(void *priv, int uah)
{
	struct qcom_fg_chip *chip = priv;
	int ret;

	if (uah <= 0)
		return -EINVAL;

	mutex_lock(&chip->cl_lock);
	ret = qcom_fg_store_learned_capacity(chip, uah);
	mutex_unlock(&chip->cl_lock);
	if (ret)
		return ret;

	mutex_lock(&chip->state_lock);
	chip->learned_cap_uah = uah;
	mutex_unlock(&chip->state_lock);

	qcom_bms_notify_changed(chip->dev);
	return 0;
}

static void qcom_fg_charging_state_changed(void *priv, int status,
					   bool charge_done, bool input_present)
{
	struct qcom_fg_chip *chip = priv;

	mutex_lock(&chip->state_lock);
	chip->status = status;
	chip->charge_done = charge_done;
	chip->input_present = input_present;
	mutex_unlock(&chip->state_lock);

	qcom_fg_algorithms_update(chip);
}

static const struct qcom_bms_fg_ops qcom_fg_ops = {
	.get_capacity = qcom_fg_op_capacity,
	.get_capacity_level = qcom_fg_op_capacity_level,
	.get_current = qcom_fg_op_current,
	.get_voltage = qcom_fg_op_voltage,
	.get_power = qcom_fg_op_power,
	.get_voltage_min_design = qcom_fg_op_voltage_min_design,
	.get_voltage_max_design = qcom_fg_op_voltage_max_design,
	.get_charge_full_design = qcom_fg_op_charge_full_design,
	.get_charge_full = qcom_fg_op_charge_full,
	.get_charge_now = qcom_fg_op_charge_now,
	.get_energy_full_design = qcom_fg_op_energy_full_design,
	.get_energy_full = qcom_fg_op_energy_full,
	.get_energy_now = qcom_fg_op_energy_now,
	.get_cycle_count = qcom_fg_op_cycle_count,
	.get_health = qcom_fg_op_health,
	.get_present = qcom_fg_op_present,
	.get_temp = qcom_fg_op_temp,
	.get_scope = qcom_fg_op_scope,
	.get_technology = qcom_fg_op_technology,
	.get_manufacturer = qcom_fg_op_manufacturer,
	.get_model_name = qcom_fg_op_model_name,
	.set_charge_full = qcom_fg_op_set_charge_full,
	.charging_state_changed = qcom_fg_charging_state_changed,
};

static void qcom_fg_read_state(struct qcom_fg_chip *chip)
{
	int val;

	if (qcom_fg_get_capacity(chip, &val) == 0) {
		mutex_lock(&chip->state_lock);
		chip->batt_soc = val;
		mutex_unlock(&chip->state_lock);
	}
}

/* Re-read the FG state, run cycle/capacity-learning bookkeeping, and notify. */
static void qcom_fg_refresh(struct qcom_fg_chip *chip)
{
	qcom_fg_read_state(chip);
	qcom_fg_algorithms_update(chip);
	qcom_bms_notify_changed(chip->dev);
}

static irqreturn_t qcom_fg_handle_soc_delta(int irq, void *data)
{
	struct qcom_fg_chip *chip = data;

	qcom_fg_refresh(chip);
	return IRQ_HANDLED;
}

static int qcom_fg_load_profile(struct qcom_fg_chip *chip)
{
	struct device *dev = chip->dev;
	struct device_node *node = dev->of_node;
	const u8 *data;
	int ret, len;
	u8 val;

	data = of_get_property(node, "qcom,fg-profile-data", &len);
	if (!data) {
		dev_info(dev,
			 "No battery profile data, skipping profile load\n");
		return 0;
	}

	if (len != PROFILE_LEN) {
		dev_err(dev,
			"Battery profile incorrect size: %d (expected %d)\n",
			len, PROFILE_LEN);
		return -EINVAL;
	}

	ret = qcom_fg_sram_write(chip, PROFILE_LOAD_WORD, 0, (u8 *)data,
				 PROFILE_LEN);
	if (ret) {
		dev_err(dev, "Failed to write profile data: %d\n", ret);
		return ret;
	}

	val = HLOS_RESTART_BIT | PROFILE_LOAD_BIT;
	ret = qcom_fg_sram_write(chip, PROFILE_INTEGRITY_WORD, 0, &val, 1);
	if (ret)
		return ret;

	dev_info(dev, "Battery profile loaded successfully\n");
	return 0;
}

static void fg_encode_ki_coeff(u8 *buf, int val)
{
	buf[0] = (u8)div_s64((s64)val * 1000, 61035);
}

static void fg_encode_ki_coeff_thresh(u8 *buf, int val)
{
	buf[0] = (u8)div_s64((s64)val * 1000, 15625);
}

static void fg_encode_voltage(u8 *buf, int val_mv)
{
	s64 raw = div_s64((s64)val_mv * 1000000, 244141);

	put_unaligned_le16((u16)raw, buf);
}

static void fg_encode_current(u8 *buf, int val_ma)
{
	s64 raw = div_s64((s64)val_ma * 100000, 48828);

	put_unaligned_le16((u16)raw, buf);
}

static void fg_encode_vbatt_low(u8 *buf, int val_mv)
{
	buf[0] = (u8)div_s64((s64)(val_mv - 2000) * 1000, 15625);
}

static int qcom_fg_sram_write_word(struct qcom_fg_chip *chip, u16 word, u8 lo,
				   u8 hi)
{
	u8 buf[2] = { lo, hi };

	return qcom_fg_sram_write(chip, word, 0, buf, 2);
}

static int qcom_fg_sram_write_byte(struct qcom_fg_chip *chip, u16 word,
				   u8 offset, u8 val)
{
	return qcom_fg_sram_write(chip, word, offset, &val, 1);
}

struct fg_u32_param {
	const char *prop;
	u16 word;
	u8 offset;
	u8 len;
	void (*encode)(u8 *buf, int val);
};

static const struct fg_u32_param fg_u32_params[] = {
	{ "qcom,fg-cutoff-voltage", CUTOFF_VOLT_WORD, 0, 2, fg_encode_voltage },
	{ "qcom,fg-cutoff-current", CUTOFF_CURR_WORD, 0, 2, fg_encode_current },
	{ "qcom,fg-empty-voltage", VBATT_LOW_WORD, 1, 1, fg_encode_vbatt_low },
	{ "qcom,fg-sys-term-current", SYS_TERM_CURR_WORD, 0, 2,
	  fg_encode_current },
	{ "qcom,ki-coeff-low-chg", KI_COEFF_LOW_CHG_WORD,
	  KI_COEFF_LOW_CHG_OFFSET, 1, fg_encode_ki_coeff },
	{ "qcom,ki-coeff-med-chg", KI_COEFF_MED_CHG_WORD,
	  KI_COEFF_MED_CHG_OFFSET, 1, fg_encode_ki_coeff },
	{ "qcom,ki-coeff-hi-chg", KI_COEFF_HI_CHG_WORD, KI_COEFF_HI_CHG_OFFSET,
	  1, fg_encode_ki_coeff },
	{ "qcom,ki-coeff-chg-low-med-thresh-ma", KI_COEFF_LO_MED_CHG_THR_WORD,
	  KI_COEFF_LO_MED_CHG_THR_OFFSET, 1, fg_encode_ki_coeff_thresh },
	{ "qcom,ki-coeff-chg-med-hi-thresh-ma", KI_COEFF_MED_HI_CHG_THR_WORD,
	  KI_COEFF_MED_HI_CHG_THR_OFFSET, 1, fg_encode_ki_coeff_thresh },
};

/*
 * DT array [init, max]; SRAM layout byte0=max, byte1=init.
 */
static const struct {
	const char *prop;
	u16 word;
} fg_esr_timers[] = {
	{ "qcom,fg-esr-timer-chg-slow", ESR_TIMER_CHG_WORD },
	{ "qcom,fg-esr-timer-dischg-slow", ESR_TIMER_DISCHG_WORD },
};

static int qcom_fg_write_u32_param(struct qcom_fg_chip *chip,
				   struct device_node *node,
				   const struct fg_u32_param *p)
{
	u32 val;
	u8 buf[2];

	if (of_property_read_u32(node, p->prop, &val))
		return 0;

	p->encode(buf, val);
	return qcom_fg_sram_write(chip, p->word, p->offset, buf, p->len);
}

static int qcom_fg_configure_params(struct qcom_fg_chip *chip)
{
	struct device *dev = chip->dev;
	struct device_node *node = dev->of_node;
	const struct fg_u32_param *p;
	u32 arr[2];
	int ret, i;

	for (i = 0; i < ARRAY_SIZE(fg_u32_params); i++) {
		p = &fg_u32_params[i];
		ret = qcom_fg_write_u32_param(chip, node, p);
		if (ret)
			return ret;
	}

	for (i = 0; i < ARRAY_SIZE(fg_esr_timers); i++) {
		if (of_property_read_u32_array(node, fg_esr_timers[i].prop, arr,
					       2))
			continue;
		ret = qcom_fg_sram_write_word(chip, fg_esr_timers[i].word,
					      (u8)arr[1], (u8)arr[0]);
		if (ret)
			return ret;
	}

	if (!of_property_read_u32_array(node, "qcom,fg-esr-cal-soc-thresh", arr,
					2)) {
		ret = qcom_fg_sram_write_byte(chip, ESR_CAL_SOC_MIN_WORD, 1,
					      arr[0]);
		if (ret)
			return ret;
		ret = qcom_fg_sram_write_byte(chip, ESR_CAL_THRESH_WORD, 0,
					      arr[1]);
		if (ret)
			return ret;
	}

	if (!of_property_read_u32_array(node, "qcom,fg-esr-cal-temp-thresh",
					arr, 2)) {
		ret = qcom_fg_sram_write_byte(chip, ESR_CAL_THRESH_WORD, 1,
					      arr[0]);
		if (ret)
			return ret;
		ret = qcom_fg_sram_write_byte(chip, ESR_PULSE_THRESH_WORD, 0,
					      arr[1]);
		if (ret)
			return ret;
	}

	dev_info(dev, "FG parameters configured successfully\n");
	return 0;
}

static int qcom_fg_init_capacity_learning(struct qcom_fg_chip *chip,
					  struct device_node *node)
{
	s64 nom_cap = 0, learned_cap;
	bool store_learned = false;
	int nom_cap_mah;
	u8 buf[2];
	int ret;

	ret = qcom_fg_sram_read(chip, NOM_CAP_WORD, 0, buf, 2);
	if (ret) {
		dev_warn(chip->dev, "Failed to read NOM_CAP from SRAM: %d\n",
			 ret);
	} else {
		nom_cap_mah = get_unaligned_le16(buf);
		nom_cap = (s64)nom_cap_mah * 1000;
	}

	if (nom_cap <= 0) {
		if (chip->batt_info->charge_full_design_uah <= 0) {
			dev_err(chip->dev,
				"NOM_CAP missing from both SRAM and DT; cannot determine battery capacity\n");
			return -EINVAL;
		}

		nom_cap = chip->batt_info->charge_full_design_uah;
		dev_info(chip->dev,
			 "NOM_CAP missing from SRAM, using DT: %lld uAh\n",
			 nom_cap);

		nom_cap_mah = (int)div_s64(nom_cap, 1000);
		put_unaligned_le16(nom_cap_mah, buf);
		ret = qcom_fg_sram_write(chip, NOM_CAP_WORD, 0, buf, 2);
		if (ret)
			dev_warn(chip->dev, "Failed to write NOM_CAP: %d\n",
				 ret);
	}

	mutex_lock(&chip->cl_lock);
	ret = qcom_fg_get_learned_capacity(chip, &learned_cap);
	if (ret || learned_cap <= 0) {
		learned_cap = nom_cap;
		store_learned = true;
	} else if ((learned_cap > nom_cap ?
			    learned_cap - nom_cap :
			    nom_cap - learned_cap) > nom_cap / 2) {
		dev_warn(
			chip->dev,
			"Learned capacity %lld uAh is outside 50%% of nominal; resetting to %lld uAh\n",
			learned_cap, nom_cap);
		learned_cap = nom_cap;
		store_learned = true;
	}

	if (store_learned) {
		ret = qcom_fg_store_learned_capacity(chip, learned_cap);
		if (ret)
			dev_warn(chip->dev,
				 "Failed to initialize learned capacity: %d\n",
				 ret);
	}
	mutex_unlock(&chip->cl_lock);

	chip->nom_cap_uah = nom_cap;
	mutex_lock(&chip->state_lock);
	chip->learned_cap_uah = (learned_cap > 0) ? learned_cap : nom_cap;
	mutex_unlock(&chip->state_lock);

	if (of_property_read_u32(node, "qcom,battery-nominal-voltage-uv",
				 &chip->nom_voltage_uv)) {
		u32 vmin = chip->batt_info->voltage_min_design_uv;
		u32 vmax = chip->batt_info->voltage_max_design_uv;

		if (vmin > 0 && vmax > 0)
			chip->nom_voltage_uv = (vmin + vmax) / 2;
		else
			chip->nom_voltage_uv = QCOM_FG_NOMINAL_VOLTAGE_FLOOR_UV;
	}

	chip->manufacturer = "Unknown";
	chip->model_name = "Unknown";
	of_property_read_string(node, "qcom,battery-manufacturer",
				&chip->manufacturer);
	of_property_read_string(node, "qcom,battery-model-name",
				&chip->model_name);

	of_property_read_u32(node, "qcom,cl-start-capacity",
			     &chip->cl_max_start_soc);
	if (!chip->cl_max_start_soc)
		chip->cl_max_start_soc = 15;

	if (of_property_read_u32(node, "qcom,cl-max-increment",
				 &chip->cl_max_cap_inc))
		chip->cl_max_cap_inc = 5;

	if (of_property_read_u32(node, "qcom,cl-max-decrement",
				 &chip->cl_max_cap_dec))
		chip->cl_max_cap_dec = 100;

	if (of_property_read_u32(node, "qcom,cl-min-temp", &chip->cl_min_temp))
		chip->cl_min_temp = DEFAULT_CL_MIN_TEMP_DECIDEGC;
	if (of_property_read_u32(node, "qcom,cl-max-temp", &chip->cl_max_temp))
		chip->cl_max_temp = DEFAULT_CL_MAX_TEMP_DECIDEGC;
	if (chip->cl_min_temp > chip->cl_max_temp) {
		dev_warn(
			chip->dev,
			"Invalid capacity-learning temperature range %d..%d; using defaults\n",
			chip->cl_min_temp, chip->cl_max_temp);
		chip->cl_min_temp = DEFAULT_CL_MIN_TEMP_DECIDEGC;
		chip->cl_max_temp = DEFAULT_CL_MAX_TEMP_DECIDEGC;
	}

	if (chip->cl_max_cap_dec > 1000) {
		dev_warn(chip->dev,
			 "cl-max-decrement %d exceeds 1000%%, clamping\n",
			 chip->cl_max_cap_dec);
		chip->cl_max_cap_dec = 1000;
	}
	if (chip->cl_max_cap_inc > 1000) {
		dev_warn(chip->dev,
			 "cl-max-increment %d exceeds 1000%%, clamping\n",
			 chip->cl_max_cap_inc);
		chip->cl_max_cap_inc = 1000;
	}

	dev_dbg(chip->dev,
		"Capacity learning initialized: nom=%lld uAh, learned=%lld uAh\n",
		chip->nom_cap_uah, chip->learned_cap_uah);
	return 0;
}

static int qcom_fg_probe(struct platform_device *pdev)
{
	struct qcom_fg_chip *chip;
	const __be32 *prop_addr;
	int irq;
	int ret;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &pdev->dev;

	init_completion(&chip->mem_attn_done);
	mutex_init(&chip->dma_lock);
	mutex_init(&chip->cl_lock);
	mutex_init(&chip->cycle_lock);
	mutex_init(&chip->state_lock);
	chip->cycle_last_bucket = -1;
	chip->status = POWER_SUPPLY_STATUS_UNKNOWN;

	chip->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!chip->regmap) {
		dev_err(chip->dev, "Failed to locate the regmap\n");
		return -ENODEV;
	}

	prop_addr = of_get_address(pdev->dev.of_node, 0, NULL, NULL);
	if (!prop_addr) {
		dev_err(chip->dev, "Failed to read SOC base address from dt\n");
		return -EINVAL;
	}
	chip->base = be32_to_cpu(*prop_addr);

	if (of_find_property(pdev->dev.of_node, "nvmem", NULL)) {
		chip->nvmem = devm_nvmem_device_get(chip->dev, "fg_sdam");
		if (IS_ERR(chip->nvmem))
			return dev_err_probe(chip->dev, PTR_ERR(chip->nvmem),
					     "Failed to get FG SDAM\n");
	}

	ret = qcom_fg_clear_dma_errors(chip);
	if (ret < 0) {
		dev_err(chip->dev, "Failed to clear DMA errors: %d\n", ret);
		return ret;
	}

	ret = qcom_fg_dma_init(chip);
	if (ret) {
		dev_err(chip->dev, "DMA init failed: %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, chip);

	ret = qcom_fg_parse_battery_info(chip);
	if (ret) {
		dev_err(&pdev->dev, "Failed to parse battery info: %d\n", ret);
		return ret;
	}

	ret = qcom_fg_load_profile(chip);
	if (ret)
		dev_warn(chip->dev, "Failed to load battery profile: %d\n",
			 ret);

	ret = qcom_fg_configure_params(chip);
	if (ret)
		dev_warn(chip->dev, "Failed to configure FG parameters: %d\n",
			 ret);

	ret = qcom_fg_init_capacity_learning(chip, pdev->dev.of_node);
	if (ret)
		return ret;

	ret = qcom_fg_restore_cycle_count(chip);
	if (ret)
		dev_warn(chip->dev, "Failed to initialize cycle count: %d\n",
			 ret);

	irq = platform_get_irq_byname(pdev, "soc-delta");
	if (irq < 0) {
		dev_err(&pdev->dev, "Failed to get soc-delta IRQ: %d\n", irq);
		ret = irq;
		return ret;
	}

	ret = devm_request_threaded_irq(chip->dev, irq, NULL,
					qcom_fg_handle_soc_delta, IRQF_ONESHOT,
					"soc-delta", chip);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to request soc-delta IRQ: %d\n",
			ret);
		return ret;
	}

	irq = platform_get_irq_byname(pdev, "mem-attn");
	if (irq <= 0) {
		dev_err(&pdev->dev, "Failed to get mem-attn IRQ: %d\n", irq);
		ret = irq ?: -ENXIO;
		return ret;
	}

	ret = devm_request_threaded_irq(chip->dev, irq, NULL,
					qcom_fg_mem_attn_irq_handler,
					IRQF_ONESHOT, "mem-attn", chip);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to request mem-attn IRQ: %d\n",
			ret);
		return ret;
	}

	irq = platform_get_irq_byname(pdev, "ima-xcp");
	if (irq > 0) {
		ret = devm_request_threaded_irq(chip->dev, irq, NULL,
						qcom_fg_mem_xcp_irq_handler,
						IRQF_ONESHOT, "ima-xcp", chip);
		if (ret < 0) {
			dev_err(&pdev->dev,
				"Failed to request ima-xcp IRQ: %d\n", ret);
			return ret;
		}
	} else if (irq != -ENXIO) {
		dev_err(&pdev->dev, "Failed to get ima-xcp IRQ: %d\n", irq);
		ret = irq;
		return ret;
	}

	ret = qcom_bms_register_fg(&pdev->dev, &qcom_fg_ops, chip);
	if (ret) {
		dev_err(chip->dev, "Failed to register with qcom_bms: %d\n",
			ret);
		return ret;
	}

	qcom_fg_read_state(chip);

	return 0;
}

static int __maybe_unused qcom_fg_resume(struct device *dev)
{
	struct qcom_fg_chip *chip = dev_get_drvdata(dev);

	qcom_fg_refresh(chip);

	return 0;
}

static SIMPLE_DEV_PM_OPS(qcom_fg_pm_ops, NULL, qcom_fg_resume);

static void qcom_fg_shutdown(struct platform_device *pdev)
{
	struct qcom_fg_chip *chip = platform_get_drvdata(pdev);
	bool input_present;
	u8 batt_soc_raw;

	if (qcom_fg_get_batt_soc(chip, NULL, &batt_soc_raw))
		return;

	mutex_lock(&chip->state_lock);
	input_present = chip->input_present;
	mutex_unlock(&chip->state_lock);

	/* Treat shutdown as the end of the current charge-throughput sample. */
	qcom_fg_cycle_count_update(chip, batt_soc_raw,
				   POWER_SUPPLY_STATUS_NOT_CHARGING, true,
				   input_present);
}

static void qcom_fg_remove(struct platform_device *pdev)
{
	qcom_bms_unregister_fg(&pdev->dev);
}

static const struct of_device_id fg_match_id_table[] = {
	{ .compatible = "qcom,pm8150b-fg" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, fg_match_id_table);

static struct platform_driver qcom_fg_driver = {
	.probe = qcom_fg_probe,
	.remove = qcom_fg_remove,
	.shutdown = qcom_fg_shutdown,
	.driver = {
		.name = "qcom-fg",
		.of_match_table = fg_match_id_table,
		.pm = &qcom_fg_pm_ops,
	},
};

module_platform_driver(qcom_fg_driver);

MODULE_AUTHOR("Caleb Connolly <caleb@connolly.tech>");
MODULE_AUTHOR("Joel Selvaraj <jo@jsfamily.in>");
MODULE_AUTHOR("Yassine Oudjana <y.oudjana@protonmail.com>");
MODULE_AUTHOR("TwinbornPlate75 <3342733415@qq.com>");
MODULE_DESCRIPTION("Qualcomm PM8150B Fuel Gauge Driver");
MODULE_LICENSE("GPL v2");
