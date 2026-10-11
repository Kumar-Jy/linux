// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/soc/qcom/llcc-qcom.h>

#include "iris_core.h"
#include "iris_firmware.h"
#include "iris_vpu_common.h"
#include "iris_vpu_register_defines.h"

#define WRAPPER_TZ_BASE_OFFS			0x000C0000
#define AON_BASE_OFFS				0x000E0000

#define CPU_IC_BASE_OFFS			(CPU_BASE_OFFS)

#define CPU_CS_A2HSOFTINTCLR			(CPU_CS_BASE_OFFS + 0x1C)
#define CLEAR_XTENSA2HOST_INTR			BIT(0)

#define CTRL_INIT				(CPU_CS_BASE_OFFS + 0x48)
#define CTRL_STATUS				(CPU_CS_BASE_OFFS + 0x4C)

#define CTRL_INIT_IDLE_MSG_BMSK			0x40000000
#define CTRL_ERROR_STATUS__M			0xfe
#define CTRL_STATUS_PC_READY			0x100

#define QTBL_INFO				(CPU_CS_BASE_OFFS + 0x50)
#define QTBL_ENABLE				BIT(0)

#define QTBL_ADDR				(CPU_CS_BASE_OFFS + 0x54)
#define CPU_CS_SCIACMDARG3			(CPU_CS_BASE_OFFS + 0x58)
#define SFR_ADDR				(CPU_CS_BASE_OFFS + 0x5C)
#define UC_REGION_ADDR				(CPU_CS_BASE_OFFS + 0x64)
#define UC_REGION_SIZE				(CPU_CS_BASE_OFFS + 0x68)

#define CPU_CS_H2XSOFTINTEN			(CPU_CS_BASE_OFFS + 0x148)
#define HOST2XTENSA_INTR_ENABLE			BIT(0)

#define CPU_CS_X2RPMH				(CPU_CS_BASE_OFFS + 0x168)
#define MSK_SIGNAL_FROM_TENSILICA		BIT(0)
#define MSK_CORE_POWER_ON			BIT(1)

#define CPU_IC_SOFTINT				(CPU_IC_BASE_OFFS + 0x150)
#define CPU_IC_SOFTINT_H2A_SHFT			0x0

#define WRAPPER_INTR_STATUS			(WRAPPER_BASE_OFFS + 0x0C)
#define WRAPPER_INTR_STATUS_A2HWD_BMSK		BIT(3)
#define WRAPPER_INTR_STATUS_A2H_BMSK		BIT(2)

#define WRAPPER_INTR_MASK			(WRAPPER_BASE_OFFS + 0x10)
#define WRAPPER_INTR_MASK_A2HWD_BMSK		BIT(3)
#define WRAPPER_INTR_MASK_A2HCPU_BMSK		BIT(2)

#define WRAPPER_DEBUG_BRIDGE_LPI_CONTROL	(WRAPPER_BASE_OFFS + 0x54)
#define WRAPPER_DEBUG_BRIDGE_LPI_STATUS		(WRAPPER_BASE_OFFS + 0x58)
#define WRAPPER_IRIS_CPU_NOC_LPI_CONTROL	(WRAPPER_BASE_OFFS + 0x5C)
#define WRAPPER_IRIS_CPU_NOC_LPI_STATUS		(WRAPPER_BASE_OFFS + 0x60)

#define WRAPPER_VCODEC0_MMCC_POWER_STATUS	(WRAPPER_BASE_OFFS + 0x90)
#define WRAPPER_VCODEC0_MMCC_POWER_CONTROL	(WRAPPER_BASE_OFFS + 0x94)

#define WRAPPER_TZ_CPU_STATUS			(WRAPPER_TZ_BASE_OFFS + 0x10)
#define WRAPPER_TZ_CTL_AXI_CLOCK_CONFIG		(WRAPPER_TZ_BASE_OFFS + 0x14)
#define CTL_AXI_CLK_HALT			BIT(0)
#define CTL_CLK_HALT				BIT(1)

#define WRAPPER_TZ_QNS4PDXFIFO_RESET		(WRAPPER_TZ_BASE_OFFS + 0x18)
#define RESET_HIGH				BIT(0)

#define AON_WRAPPER_MVP_NOC_LPI_CONTROL		(AON_BASE_OFFS)
#define REQ_POWER_DOWN_PREP			BIT(0)

#define AON_WRAPPER_MVP_NOC_LPI_STATUS		(AON_BASE_OFFS + 0x4)

/* Legacy VPU5 register layout used by SM8150. */
#define VPU5_CPU_CS_BASE_OFFS			0x000D2000
#define VPU5_CPU_IC_BASE_OFFS			0x000DF000
#define VPU5_WRAPPER_BASE_OFFS			0x000E0000

static bool iris_vpu_uses_legacy_vpu5(struct iris_core *core)
{
	return core->iris_platform_data->legacy_vpu5;
}

static u32 iris_vpu_cpu_cs_base(struct iris_core *core)
{
	if (iris_vpu_uses_legacy_vpu5(core))
		return VPU5_CPU_CS_BASE_OFFS;

	return CPU_CS_BASE_OFFS;
}

static u32 iris_vpu_wrapper_base(struct iris_core *core)
{
	if (iris_vpu_uses_legacy_vpu5(core))
		return VPU5_WRAPPER_BASE_OFFS;

	return WRAPPER_BASE_OFFS;
}

static void iris_vpu_interrupt_init(struct iris_core *core)
{
	u32 wrapper_base = iris_vpu_wrapper_base(core);
	u32 mask_val;

	if (iris_vpu_uses_legacy_vpu5(core)) {
		/*
		 * Iris1 masks reserved interrupt sources at reset.  Unmask only
		 * the normal A2H CPU interrupt and keep the firmware watchdog
		 * (BIT(4)) masked, as an uncleared level interrupt would stall
		 * the host CPU.
		 */
		mask_val = readl(core->reg_base + wrapper_base + 0x10);
		dev_info(core->dev,
			 "Iris1 interrupt mask before unmask: %#x\n",
			 mask_val);
		mask_val |= BIT(4);
		mask_val &= ~BIT(2);
	} else {
		mask_val = readl(core->reg_base + wrapper_base + 0x10);
		mask_val &= ~(BIT(3) | BIT(2));
	}
	writel(mask_val, core->reg_base + wrapper_base + 0x10);
	if (iris_vpu_uses_legacy_vpu5(core))
		dev_info(core->dev,
			 "Iris1 interrupt mask after unmask: %#x\n",
			 mask_val);
}

static void iris_vpu_setup_ucregion_memory_map(struct iris_core *core)
{
	u32 cpu_cs_base = iris_vpu_cpu_cs_base(core);
	u32 queue_size, region_size, value;
	u32 queue_table_size;

	/* Iris hardware requires 4K queue alignment */
	queue_table_size = sizeof(struct iris_hfi_queue_table_header);
	queue_size = ALIGN(queue_table_size +
			  (IFACEQ_QUEUE_SIZE * IFACEQ_NUMQ), SZ_4K);

	value = (u32)core->iface_q_table_daddr;
	dev_info(core->dev, "HFI registers: writing UC_REGION_ADDR %#x to %#x\n",
		 value, cpu_cs_base + 0x64);
	writel(value, core->reg_base + cpu_cs_base + 0x64);
	dev_info(core->dev, "HFI registers: UC_REGION_ADDR write complete\n");

	/* Iris hardware requires 1M queue alignment */
	region_size = ALIGN(SFR_SIZE + queue_size, SZ_1M);
	dev_info(core->dev, "HFI registers: writing UC_REGION_SIZE %#x to %#x\n",
		 region_size, cpu_cs_base + 0x68);
	writel(region_size, core->reg_base + cpu_cs_base + 0x68);
	dev_info(core->dev, "HFI registers: UC_REGION_SIZE write complete\n");

	value = (u32)core->iface_q_table_daddr;
	dev_info(core->dev, "HFI registers: writing QTBL_ADDR %#x to %#x\n",
		 value, cpu_cs_base + 0x54);
	writel(value, core->reg_base + cpu_cs_base + 0x54);
	dev_info(core->dev, "HFI registers: QTBL_ADDR write complete\n");

	dev_info(core->dev, "HFI registers: enabling QTBL at %#x\n",
		 cpu_cs_base + 0x50);
	writel(QTBL_ENABLE, core->reg_base + cpu_cs_base + 0x50);
	dev_info(core->dev, "HFI registers: QTBL enable complete\n");

	if (iris_vpu_uses_legacy_vpu5(core)) {
		/*
		 * Iris1 has a second HFI queue view for its DSP.  Qualcomm's
		 * downstream SM8150 driver initializes it with the CPU queues
		 * by default even when CVP is unused.
		 */
		value = (u32)core->iface_q_table_daddr;
		dev_info(core->dev,
			 "HFI registers: writing DSP QTBL/UC region %#x size %#x\n",
			 value, region_size);
		writel(value, core->reg_base + cpu_cs_base + 0x34);
		writel(value, core->reg_base + cpu_cs_base + 0x38);
		writel(region_size, core->reg_base + cpu_cs_base + 0x3c);
		dev_info(core->dev,
			 "HFI registers: DSP QTBL/UC region writes complete\n");
	}

	if (core->sfr_daddr) {
		value = (u32)core->sfr_daddr + core->iris_platform_data->core_arch;
		dev_info(core->dev, "HFI registers: writing SFR_ADDR %#x to %#x\n",
			 value, cpu_cs_base + 0x5c);
		writel(value, core->reg_base + cpu_cs_base + 0x5c);
		dev_info(core->dev, "HFI registers: SFR_ADDR write complete\n");
	}
}

int iris_vpu_boot_firmware(struct iris_core *core)
{
	u32 cpu_cs_base = iris_vpu_cpu_cs_base(core);
	u32 ctrl_init = BIT(0), ctrl_status = 0, count = 0, max_tries = 1000;

	iris_vpu_setup_ucregion_memory_map(core);

	dev_info(core->dev, "HFI registers: writing CTRL_INIT to %#x\n",
		 cpu_cs_base + 0x48);
	writel(ctrl_init, core->reg_base + cpu_cs_base + 0x48);
	dev_info(core->dev, "HFI registers: CTRL_INIT write complete\n");

	dev_info(core->dev, "HFI registers: polling CTRL_STATUS at %#x\n",
		 cpu_cs_base + 0x4c);
	while (!ctrl_status && count < max_tries) {
		ctrl_status = readl(core->reg_base + cpu_cs_base + 0x4c);
		if ((ctrl_status & CTRL_ERROR_STATUS__M) == 0x4) {
			dev_err(core->dev, "invalid setting for uc_region\n");
			break;
		}

		usleep_range(50, 100);
		count++;
	}
	dev_info(core->dev, "HFI registers: CTRL_STATUS %#x after %u polls\n",
		 ctrl_status, count);

	if (count >= max_tries) {
		dev_err(core->dev, "error booting up iris firmware\n");
		return -ETIME;
	}

	/*
	 * The SM8150 VPU5 downstream driver stops here.  H2XSOFTINTEN and
	 * X2RPMH belong to the newer Iris register layout and must not be
	 * programmed through the VPU5 CPU_CS window.
	 */
	if (iris_vpu_uses_legacy_vpu5(core)) {
		dev_info(core->dev,
			 "Iris1 firmware booted; leaving Iris2-only controls untouched\n");
		return 0;
	}

	writel(0x1, core->reg_base + cpu_cs_base + 0x58);
	writel(HOST2XTENSA_INTR_ENABLE,
	       core->reg_base + CPU_CS_H2XSOFTINTEN);
	writel(0x0, core->reg_base + CPU_CS_X2RPMH);

	return 0;
}

void iris_vpu_raise_interrupt(struct iris_core *core)
{
	if (iris_vpu_uses_legacy_vpu5(core)) {
		dev_dbg(core->dev,
			 "HFI doorbell: writing BIT(15) to VPU5 CPU_IC %#x\n",
			 VPU5_CPU_IC_BASE_OFFS + 0x18);
		writel(BIT(15), core->reg_base + VPU5_CPU_IC_BASE_OFFS + 0x18);
		dev_dbg(core->dev, "HFI doorbell: write returned\n");
	} else {
		writel(BIT(CPU_IC_SOFTINT_H2A_SHFT),
		       core->reg_base + CPU_IC_SOFTINT);
	}
}

void iris_vpu_clear_interrupt(struct iris_core *core)
{
	u32 cpu_cs_base = iris_vpu_cpu_cs_base(core);
	u32 wrapper_base = iris_vpu_wrapper_base(core);
	u32 intr_status, mask;

	intr_status = readl(core->reg_base + wrapper_base + 0x0c);
	mask = BIT(2) | CTRL_INIT_IDLE_MSG_BMSK;
	if (iris_vpu_uses_legacy_vpu5(core))
		mask |= BIT(4);
	else
		mask |= BIT(3);

	if (intr_status & mask)
		core->intr_status |= intr_status;

	writel(CLEAR_XTENSA2HOST_INTR, core->reg_base + cpu_cs_base + 0x1c);
	if (iris_vpu_uses_legacy_vpu5(core))
		writel(intr_status, core->reg_base + wrapper_base + 0x14);
}

int iris_vpu_watchdog(struct iris_core *core, u32 intr_status)
{
	u32 watchdog_mask = iris_vpu_uses_legacy_vpu5(core) ? BIT(4) : BIT(3);

	if (intr_status & watchdog_mask) {
		dev_err(core->dev, "received watchdog interrupt\n");
		return -ETIME;
	}

	return 0;
}

int iris_vpu_prepare_pc(struct iris_core *core)
{
	u32 wfi_status, idle_status, pc_ready;
	u32 ctrl_status, val = 0;
	int ret;

	/*
	 * The Iris2 power-collapse sequence below addresses registers that do
	 * not exist in the SM8150 VPU5 layout.  Keep the controller on while
	 * bringing up SM8150; streaming power collapse can be added separately.
	 */
	if (iris_vpu_uses_legacy_vpu5(core))
		return -EAGAIN;

	ctrl_status = readl(core->reg_base + CTRL_STATUS);
	pc_ready = ctrl_status & CTRL_STATUS_PC_READY;
	idle_status = ctrl_status & BIT(30);
	if (pc_ready)
		return 0;

	wfi_status = readl(core->reg_base + WRAPPER_TZ_CPU_STATUS);
	wfi_status &= BIT(0);
	if (!wfi_status || !idle_status)
		goto skip_power_off;

	ret = core->hfi_ops->sys_pc_prep(core);
	if (ret)
		goto skip_power_off;

	ret = readl_poll_timeout(core->reg_base + CTRL_STATUS, val,
				 val & CTRL_STATUS_PC_READY, 250, 2500);
	if (ret)
		goto skip_power_off;

	ret = readl_poll_timeout(core->reg_base + WRAPPER_TZ_CPU_STATUS,
				 val, val & BIT(0), 250, 2500);
	if (ret)
		goto skip_power_off;

	return 0;

skip_power_off:
	ctrl_status = readl(core->reg_base + CTRL_STATUS);
	wfi_status = readl(core->reg_base + WRAPPER_TZ_CPU_STATUS);
	wfi_status &= BIT(0);
	dev_err(core->dev, "skip power collapse, wfi=%#x, idle=%#x, pcr=%#x, ctrl=%#x)\n",
		wfi_status, idle_status, pc_ready, ctrl_status);

	return -EAGAIN;
}

int iris_vpu_power_off_controller(struct iris_core *core)
{
	struct device *pd_dev =
		core->pmdomain_tbl->pd_devs[IRIS_CTRL_POWER_DOMAIN];
	u32 val = 0;
	int ret;

	if (iris_vpu_uses_legacy_vpu5(core)) {
		iris_disable_unprepare_clock(core, IRIS_CTRL_CLK);
		iris_disable_unprepare_clock(core, IRIS_AXI_CLK);
		if (core->iris_platform_data->defer_vcodec_power)
			iris_disable_unprepare_clock(core, IRIS_BUS_CLK);
		pm_runtime_put_sync(pd_dev);

		return 0;
	}

	writel(MSK_SIGNAL_FROM_TENSILICA | MSK_CORE_POWER_ON, core->reg_base + CPU_CS_X2RPMH);

	writel(REQ_POWER_DOWN_PREP, core->reg_base + AON_WRAPPER_MVP_NOC_LPI_CONTROL);

	ret = readl_poll_timeout(core->reg_base + AON_WRAPPER_MVP_NOC_LPI_STATUS,
				 val, val & BIT(0), 200, 2000);
	if (ret)
		goto disable_power;

	writel(REQ_POWER_DOWN_PREP, core->reg_base + WRAPPER_IRIS_CPU_NOC_LPI_CONTROL);

	ret = readl_poll_timeout(core->reg_base + WRAPPER_IRIS_CPU_NOC_LPI_STATUS,
				 val, val & BIT(0), 200, 2000);
	if (ret)
		goto disable_power;

	writel(0x0, core->reg_base + WRAPPER_DEBUG_BRIDGE_LPI_CONTROL);

	ret = readl_poll_timeout(core->reg_base + WRAPPER_DEBUG_BRIDGE_LPI_STATUS,
				 val, val == 0, 200, 2000);
	if (ret)
		goto disable_power;

	writel(CTL_AXI_CLK_HALT | CTL_CLK_HALT,
	       core->reg_base + WRAPPER_TZ_CTL_AXI_CLOCK_CONFIG);
	writel(RESET_HIGH, core->reg_base + WRAPPER_TZ_QNS4PDXFIFO_RESET);
	writel(0x0, core->reg_base + WRAPPER_TZ_QNS4PDXFIFO_RESET);
	writel(0x0, core->reg_base + WRAPPER_TZ_CTL_AXI_CLOCK_CONFIG);

disable_power:
	iris_disable_unprepare_clock(core, IRIS_CTRL_CLK);
	iris_disable_unprepare_clock(core, IRIS_AXI_CLK);
	if (core->iris_platform_data->defer_vcodec_power)
		iris_disable_unprepare_clock(core, IRIS_BUS_CLK);
	iris_disable_power_domains(core, pd_dev);

	return 0;
}

static int iris_vpu_set_vcodec_hw_control(struct iris_core *core, bool enable)
{
	u32 val;

	writel(enable ? 0 : 1,
	       core->reg_base + WRAPPER_VCODEC0_MMCC_POWER_CONTROL);

	return readl_poll_timeout(core->reg_base +
				  WRAPPER_VCODEC0_MMCC_POWER_STATUS,
				  val,
				  enable ? (val & BIT(1)) : !(val & BIT(1)),
				  1, 100);
}

void iris_vpu_power_off_hw(struct iris_core *core)
{
	struct device *pd_dev =
		core->pmdomain_tbl->pd_devs[IRIS_HW_POWER_DOMAIN];
	struct device *aux_pd_dev = NULL;

	if (core->iris_platform_data->defer_vcodec_power &&
	    core->iris_platform_data->pmdomain_tbl_size >
		    IRIS_AUX_HW_POWER_DOMAIN)
		aux_pd_dev =
			core->pmdomain_tbl->pd_devs[IRIS_AUX_HW_POWER_DOMAIN];

	if (core->iris_platform_data->use_vcodec_hw_control &&
	    !core->iris_platform_data->defer_vcodec_power) {
		if (iris_vpu_set_vcodec_hw_control(core, true))
			dev_warn(core->dev,
				 "failed to take vcodec power control\n");

		iris_disable_unprepare_clock(core, IRIS_HW_CLK);

		if (iris_vpu_set_vcodec_hw_control(core, false))
			dev_warn(core->dev,
				 "failed to release vcodec power control\n");
	} else if (core->iris_platform_data->defer_vcodec_power) {
		/*
		 * SM8150 VideoCC uses HW_CTRL_TRIGGER, so pm_runtime_get()
		 * leaves both MVS GDSCs in software mode.  Keep them there for
		 * now: touching the legacy wrapper handoff registers wedges
		 * nabu before the codec clocks are running.
		 */
		if (aux_pd_dev)
			iris_disable_unprepare_clock(core, IRIS_AUX_HW_CLK);
		iris_disable_unprepare_clock(core, IRIS_HW_CLK);
		if (aux_pd_dev)
			iris_disable_unprepare_clock(core, IRIS_AUX_HW_AXI_CLK);
		iris_disable_unprepare_clock(core, IRIS_HW_AXI_CLK);
	} else {
		dev_pm_genpd_set_hwmode(pd_dev, false);
		iris_disable_unprepare_clock(core, IRIS_HW_CLK);
	}

	if (core->iris_platform_data->defer_vcodec_power) {
		if (aux_pd_dev)
			pm_runtime_put_sync(aux_pd_dev);
		pm_runtime_put_sync(pd_dev);
	} else {
		iris_disable_power_domains(core, pd_dev);
	}
}

void iris_vpu_power_off(struct iris_core *core)
{
	int i;

	if (core->llcc_active) {
		for (i = ARRAY_SIZE(core->llcc_slices) - 1; i >= 0; i--) {
			llcc_slice_deactivate(core->llcc_slices[i]);
			llcc_slice_putd(core->llcc_slices[i]);
			core->llcc_slices[i] = NULL;
		}
		core->llcc_active = false;
		core->syscache_set = false;
	}

	dev_pm_opp_set_rate(core->dev, 0);
	core->iris_platform_data->vpu_ops->power_off_hw(core);
	core->iris_platform_data->vpu_ops->power_off_controller(core);
	iris_unset_icc_bw(core);

	if (!iris_vpu_watchdog(core, core->intr_status))
		disable_irq_nosync(core->irq);
}

static int iris_vpu_power_on_controller(struct iris_core *core)
{
	u32 rst_tbl_size = core->iris_platform_data->clk_rst_tbl_size;
	struct device *pd_dev =
		core->pmdomain_tbl->pd_devs[IRIS_CTRL_POWER_DOMAIN];
	u32 i;
	int ret;

	dev_info(core->dev, "vpu controller: enabling power domain\n");
	if (core->iris_platform_data->defer_vcodec_power)
		ret = pm_runtime_get_sync(pd_dev);
	else
		ret = iris_enable_power_domains(core, pd_dev);
	if (ret < 0)
		return ret;

	dev_info(core->dev, "vpu controller: resetting clocks\n");
	if (core->iris_platform_data->defer_vcodec_power) {
		for (i = 0; i < rst_tbl_size; i++) {
			dev_info(core->dev, "vpu controller: reset %u (%s): assert begin\n",
				 i, core->resets[i].id);
			ret = reset_control_assert(core->resets[i].rstc);
			if (ret)
				goto err_disable_power;
			dev_info(core->dev, "vpu controller: reset %u (%s): assert done\n",
				 i, core->resets[i].id);

			usleep_range(150, 250);

			dev_info(core->dev, "vpu controller: reset %u (%s): deassert begin\n",
				 i, core->resets[i].id);
			ret = reset_control_deassert(core->resets[i].rstc);
			if (ret)
				goto err_disable_power;
			dev_info(core->dev, "vpu controller: reset %u (%s): deassert done\n",
				 i, core->resets[i].id);
		}
	} else {
		ret = reset_control_bulk_reset(rst_tbl_size, core->resets);
		if (ret)
			goto err_disable_power;
	}

	if (core->iris_platform_data->defer_vcodec_power) {
		dev_info(core->dev, "vpu controller: enabling bus clock\n");
		ret = iris_prepare_enable_clock(core, IRIS_BUS_CLK);
		if (ret)
			goto err_disable_power;
	}

	dev_info(core->dev, "vpu controller: enabling AXI clock\n");
	ret = iris_prepare_enable_clock(core, IRIS_AXI_CLK);
	if (ret)
		goto err_disable_bus_clock;

	dev_info(core->dev, "vpu controller: enabling core clock\n");
	ret = iris_prepare_enable_clock(core, IRIS_CTRL_CLK);
	if (ret)
		goto err_disable_clock;

	dev_info(core->dev, "vpu controller: power-on complete\n");
	return 0;

err_disable_clock:
	iris_disable_unprepare_clock(core, IRIS_AXI_CLK);
err_disable_bus_clock:
	if (core->iris_platform_data->defer_vcodec_power)
		iris_disable_unprepare_clock(core, IRIS_BUS_CLK);
err_disable_power:
	if (core->iris_platform_data->defer_vcodec_power)
		pm_runtime_put_sync(pd_dev);
	else
		iris_disable_power_domains(core, pd_dev);

	return ret;
}

static int iris_vpu_power_on_hw(struct iris_core *core)
{
	struct device *pd_dev =
		core->pmdomain_tbl->pd_devs[IRIS_HW_POWER_DOMAIN];
	struct device *aux_pd_dev = NULL;
	int ret;

	if (core->iris_platform_data->defer_vcodec_power &&
	    core->iris_platform_data->pmdomain_tbl_size >
		    IRIS_AUX_HW_POWER_DOMAIN)
		aux_pd_dev =
			core->pmdomain_tbl->pd_devs[IRIS_AUX_HW_POWER_DOMAIN];

	/*
	 * The deferred SM8150 sequence has already raised the CX OPP before
	 * reaching this point.  Do not call iris_enable_power_domains() here:
	 * it repeats dev_pm_opp_set_rate(ULONG_MAX), which can wedge the RPMh
	 * TCS while VideoCC/MMCX is active.  The legacy Venus driver likewise
	 * acquires the vcodec GDSC directly at this stage.
	 */
	if (core->iris_platform_data->defer_vcodec_power) {
		dev_info(core->dev,
			 "vcodec power: enabling GDSC without duplicate OPP vote\n");
		ret = pm_runtime_resume_and_get(pd_dev);
		dev_info(core->dev, "vcodec power: GDSC enable returned %d\n", ret);
	} else {
		ret = iris_enable_power_domains(core, pd_dev);
	}
	if (ret)
		return ret;

	if (aux_pd_dev) {
		dev_info(core->dev,
			 "vcodec power: enabling auxiliary GDSC\n");
		ret = pm_runtime_resume_and_get(aux_pd_dev);
		dev_info(core->dev,
			 "vcodec power: auxiliary GDSC enable returned %d\n",
			 ret);
		if (ret)
			goto err_disable_power;
	}

	if (core->iris_platform_data->defer_vcodec_power) {
		dev_info(core->dev, "vcodec power: enabling codec0 AXI clock\n");
		ret = iris_prepare_enable_clock(core, IRIS_HW_AXI_CLK);
		if (ret)
			goto err_disable_aux_power;
		dev_info(core->dev, "vcodec power: enabling codec1 AXI clock\n");
		ret = iris_prepare_enable_clock(core, IRIS_AUX_HW_AXI_CLK);
		if (ret)
			goto err_disable_hw_axi_clock;
	}

	if (core->iris_platform_data->use_vcodec_hw_control &&
	    !core->iris_platform_data->defer_vcodec_power) {
		dev_info(core->dev, "vcodec power: requesting software control\n");
		ret = iris_vpu_set_vcodec_hw_control(core, true);
		if (ret)
			goto err_disable_aux_hw_axi_clock;
		dev_info(core->dev, "vcodec power: software control acquired\n");
	}

	dev_info(core->dev, "vcodec power: enabling core clock\n");
	ret = iris_prepare_enable_clock(core, IRIS_HW_CLK);
	if (ret)
		goto err_disable_aux_hw_axi_clock;
	dev_info(core->dev, "vcodec power: core clock enabled\n");

	if (aux_pd_dev) {
		dev_info(core->dev,
			 "vcodec power: enabling auxiliary core clock\n");
		ret = iris_prepare_enable_clock(core, IRIS_AUX_HW_CLK);
		if (ret)
			goto err_disable_clock;
		dev_info(core->dev,
			 "vcodec power: auxiliary core clock enabled\n");
	}

	if (core->iris_platform_data->use_vcodec_hw_control &&
	    !core->iris_platform_data->defer_vcodec_power) {
		dev_info(core->dev, "vcodec power: releasing software control\n");
		ret = iris_vpu_set_vcodec_hw_control(core, false);
	} else if (core->iris_platform_data->defer_vcodec_power) {
		/*
		 * Keep vcodec0 software-powered for the first SM8150 bring-up.
		 * This deliberately trades power collapse for a stable decode
		 * path and avoids the inaccessible legacy wrapper registers.
		 */
		dev_info(core->dev,
			 "vcodec power: keeping both GDSCs in software mode\n");
		ret = 0;
	} else {
		ret = dev_pm_genpd_set_hwmode(pd_dev, true);
	}
	if (ret)
		goto err_disable_clock;
	dev_info(core->dev, "vcodec power: hardware ready\n");

	return 0;

err_disable_clock:
	if (aux_pd_dev)
		iris_disable_unprepare_clock(core, IRIS_AUX_HW_CLK);
	iris_disable_unprepare_clock(core, IRIS_HW_CLK);
err_disable_aux_hw_axi_clock:
	if (core->iris_platform_data->defer_vcodec_power)
		iris_disable_unprepare_clock(core, IRIS_AUX_HW_AXI_CLK);
err_disable_hw_axi_clock:
	if (core->iris_platform_data->defer_vcodec_power)
		iris_disable_unprepare_clock(core, IRIS_HW_AXI_CLK);
err_disable_aux_power:
	if (aux_pd_dev)
		pm_runtime_put_sync(aux_pd_dev);
	if (core->iris_platform_data->use_vcodec_hw_control &&
	    !core->iris_platform_data->defer_vcodec_power)
		iris_vpu_set_vcodec_hw_control(core, false);
err_disable_power:
	if (core->iris_platform_data->defer_vcodec_power)
		pm_runtime_put_sync(pd_dev);
	else
		iris_disable_power_domains(core, pd_dev);

	return ret;
}

int iris_vpu_power_on(struct iris_core *core)
{
	u32 freq;
	int ret;

	dev_info(core->dev, "vpu power: voting interconnect\n");
	ret = iris_set_icc_bw(core, INT_MAX);
	if (ret)
		goto err;
	if (core->iris_platform_data->legacy_vpu5)
		dev_info(core->dev,
			 "Iris1 v154: retaining maximum clock and interconnect votes until power-off\n");

	dev_info(core->dev, "vpu power: enabling controller domain and clocks\n");
	ret = iris_vpu_power_on_controller(core);
	if (ret)
		goto err_unvote_icc;

	if (!core->iris_platform_data->defer_vcodec_power) {
		dev_info(core->dev, "vpu power: enabling codec domain and clock\n");
		ret = iris_vpu_power_on_hw(core);
		if (ret)
			goto err_power_off_ctrl;
	} else {
		dev_info(core->dev,
			 "vpu power: codec domain deferred until after OPP vote\n");
	}

	freq = core->power.clk_freq ? core->power.clk_freq :
				      (u32)ULONG_MAX;

	dev_info(core->dev, "vpu power: setting OPP rate\n");
	ret = dev_pm_opp_set_rate(core->dev, freq);
	if (ret)
		goto err_power_off_ctrl;

	if (core->iris_platform_data->defer_vcodec_power) {
		dev_info(core->dev,
			 "vpu power: enabling codec domain after OPP vote\n");
		ret = iris_vpu_power_on_hw(core);
		if (ret)
			goto err_clear_opp;
	}

	dev_info(core->dev, "vpu power: programming preset registers\n");
	core->iris_platform_data->set_preset_registers(core);

	dev_info(core->dev, "vpu power: enabling interrupt\n");
	iris_vpu_interrupt_init(core);
	core->intr_status = 0;
	enable_irq(core->irq);

	return 0;

err_clear_opp:
	dev_pm_opp_set_rate(core->dev, 0);
err_power_off_ctrl:
	iris_vpu_power_off_controller(core);
err_unvote_icc:
	iris_unset_icc_bw(core);
err:
	dev_err(core->dev, "power on failed\n");

	return ret;
}
