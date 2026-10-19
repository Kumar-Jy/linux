/*
 * Copyright (C) 2010 - 2018 Novatek, Inc.
 * Copyright (C) 2021 XiaoMi, Inc.
 *
 * $Revision: 73033 $
 * $Date: 2020-11-26 10:09:14 +0800 (週四, 26 十一月 2020) $
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/proc_fs.h>
#include <linux/input/mt.h>
#include <linux/debugfs.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>

#ifdef CONFIG_DRM
#include <drm/drm_notifier.h>
#endif

#include "nt36xxx.h"

struct nvt_ts_data *ts;

#ifdef CONFIG_DRM
static int nvt_drm_notifier_callback(struct notifier_block *self,
				     unsigned long event, void *data);
#endif

static int32_t nvt_ts_suspend(struct device *dev);
static int32_t nvt_ts_resume(struct device *dev);

uint32_t ENG_RST_ADDR = 0x7FFF80;
uint32_t SWRST_N8_ADDR = 0; //read from dtsi
uint32_t SPI_RD_FAST_ADDR = 0; //read from dtsi

static uint8_t bTouchIsAwake = 0;

/*******************************************************
Description:
	Novatek touchscreen irq enable/disable function.

return:
	n.a.
*******************************************************/
static void nvt_irq_enable(bool enable)
{
	if (enable) {
		if (!ts->irq_enabled) {
			enable_irq(ts->client->irq);
			ts->irq_enabled = true;
		}
	} else {
		if (ts->irq_enabled) {
			disable_irq(ts->client->irq);
			ts->irq_enabled = false;
		}
	}
}

static inline int32_t nvt_spi_read(struct spi_device *client, uint8_t *buf,
				   size_t len)
{
	struct spi_message m;
	struct spi_transfer t = {
		.len = len + DUMMY_BYTES,
		.tx_buf = ts->xbuf,
		.rx_buf = ts->rbuf,
	};

	memset(ts->xbuf, 0, len + DUMMY_BYTES);
	ts->xbuf[0] = buf[0];

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	return spi_sync(client, &m);
}

static inline int32_t nvt_spi_write(struct spi_device *client, uint8_t *buf,
				    size_t len)
{
	struct spi_message m;
	struct spi_transfer t = {
		.len = len,
		.tx_buf = ts->xbuf,
	};

	memcpy(ts->xbuf, buf, len);

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	return spi_sync(client, &m);
}

/*******************************************************
Description:
	Novatek touchscreen spi read function.

return:
	Executive outcomes. 2---succeed. -5---I/O error
*******************************************************/
int32_t CTP_SPI_READ(struct spi_device *client, uint8_t *buf, uint16_t len)
{
	int32_t ret = -1;
	int32_t retries = 0;

	mutex_lock(&ts->xbuf_lock);

	buf[0] = SPI_READ_MASK(buf[0]);

	while (retries < 5) {
		ret = nvt_spi_read(client, buf, len);
		if (ret == 0)
			break;
		retries++;
	}

	if (unlikely(retries == 5)) {
		NVT_ERR("read error, ret=%d\n", ret);
		ret = -EIO;
	} else {
		memcpy((buf + 1), (ts->rbuf + 2), (len - 1));
	}

	mutex_unlock(&ts->xbuf_lock);

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen spi write function.

return:
	Executive outcomes. 1---succeed. -5---I/O error
*******************************************************/
int32_t CTP_SPI_WRITE(struct spi_device *client, uint8_t *buf, uint16_t len)
{
	int32_t ret = -1;
	int32_t retries = 0;

	mutex_lock(&ts->xbuf_lock);

	buf[0] = SPI_WRITE_MASK(buf[0]);

	while (retries < 5) {
		ret = nvt_spi_write(client, buf, len);
		if (ret == 0)
			break;
		retries++;
	}

	if (unlikely(retries == 5)) {
		NVT_ERR("error, ret=%d\n", ret);
		ret = -EIO;
	}

	mutex_unlock(&ts->xbuf_lock);

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen set index/page/addr address.

return:
	Executive outcomes. 0---succeed. -5---access fail.
*******************************************************/
int32_t nvt_set_page(uint32_t addr)
{
	uint8_t buf[4] = { 0 };

	buf[0] = 0xFF; //set index/page/addr command
	buf[1] = (addr >> 15) & 0xFF;
	buf[2] = (addr >> 7) & 0xFF;

	return CTP_SPI_WRITE(ts->client, buf, 3);
}

/*******************************************************
Description:
	Novatek touchscreen write data to specify address.

return:
	Executive outcomes. 0---succeed. -5---access fail.
*******************************************************/
int32_t nvt_write_addr(uint32_t addr, uint8_t data)
{
	int32_t ret = 0;
	uint8_t buf[4] = { 0 };

	//---set xdata index---
	buf[0] = 0xFF; //set index/page/addr command
	buf[1] = (addr >> 15) & 0xFF;
	buf[2] = (addr >> 7) & 0xFF;
	ret = CTP_SPI_WRITE(ts->client, buf, 3);
	if (ret) {
		NVT_ERR("set page 0x%06X failed, ret = %d\n", addr, ret);
		return ret;
	}

	//---write data to index---
	buf[0] = addr & (0x7F);
	buf[1] = data;
	ret = CTP_SPI_WRITE(ts->client, buf, 2);
	if (ret) {
		NVT_ERR("write data to 0x%06X failed, ret = %d\n", addr, ret);
		return ret;
	}

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen enable hw bld crc function.

return:
	N/A.
*******************************************************/
void nvt_bld_crc_enable(void)
{
	uint8_t buf[4] = { 0 };

	//---set xdata index to BLD_CRC_EN_ADDR---
	nvt_set_page(ts->mmap->BLD_CRC_EN_ADDR);

	//---read data from index---
	buf[0] = ts->mmap->BLD_CRC_EN_ADDR & (0x7F);
	buf[1] = 0xFF;
	CTP_SPI_READ(ts->client, buf, 2);

	//---write data to index---
	buf[0] = ts->mmap->BLD_CRC_EN_ADDR & (0x7F);
	buf[1] = buf[1] | (0x01 << 7);
	CTP_SPI_WRITE(ts->client, buf, 2);
}

/*******************************************************
Description:
	Novatek touchscreen clear status & enable fw crc function.

return:
	N/A.
*******************************************************/
void nvt_fw_crc_enable(void)
{
	uint8_t buf[4] = { 0 };

	//---set xdata index to EVENT BUF ADDR---
	nvt_set_page(ts->mmap->EVENT_BUF_ADDR);

	//---clear fw reset status---
	buf[0] = EVENT_MAP_RESET_COMPLETE & (0x7F);
	buf[1] = 0x00;
	CTP_SPI_WRITE(ts->client, buf, 2);

	//---enable fw crc---
	buf[0] = EVENT_MAP_HOST_CMD & (0x7F);
	buf[1] = 0xAE; //enable fw crc command
	CTP_SPI_WRITE(ts->client, buf, 2);
}

/*******************************************************
Description:
	Novatek touchscreen set boot ready function.

return:
	N/A.
*******************************************************/
void nvt_boot_ready(void)
{
	//---write BOOT_RDY status cmds---
	nvt_write_addr(ts->mmap->BOOT_RDY_ADDR, 1);

	mdelay(5);

	if (!ts->hw_crc) {
		//---write BOOT_RDY status cmds---
		nvt_write_addr(ts->mmap->BOOT_RDY_ADDR, 0);

		//---write POR_CD cmds---
		nvt_write_addr(ts->mmap->POR_CD_ADDR, 0xA0);
	}
}

/*******************************************************
Description:
	Novatek touchscreen enable auto copy mode function.

return:
	N/A.
*******************************************************/
void nvt_tx_auto_copy_mode(void)
{
	//---write TX_AUTO_COPY_EN cmds---
	nvt_write_addr(ts->mmap->TX_AUTO_COPY_EN, 0x69);

	NVT_LOG("tx auto copy mode enable\n");
}

/*******************************************************
Description:
	Novatek touchscreen check spi dma tx info function.

return:
	N/A.
*******************************************************/
int32_t nvt_check_spi_dma_tx_info(void)
{
	uint8_t buf[8] = { 0 };
	int32_t i = 0;
	const int32_t retry = 200;

	for (i = 0; i < retry; i++) {
		//---set xdata index to EVENT BUF ADDR---
		nvt_set_page(ts->mmap->SPI_DMA_TX_INFO);

		//---read fw status---
		buf[0] = ts->mmap->SPI_DMA_TX_INFO & 0x7F;
		buf[1] = 0xFF;
		CTP_SPI_READ(ts->client, buf, 2);

		if (buf[1] == 0x00)
			break;

		usleep_range(1000, 1000);
	}

	if (i >= retry) {
		NVT_ERR("failed, i=%d, buf[1]=0x%02X\n", i, buf[1]);
		return -1;
	} else {
		return 0;
	}
}

/*******************************************************
Description:
	Novatek touchscreen eng reset cmd
    function.

return:
	n.a.
*******************************************************/
void nvt_eng_reset(void)
{
	//---eng reset cmds to ENG_RST_ADDR---
	nvt_write_addr(ENG_RST_ADDR, 0x5A);

	mdelay(1); //wait tMCU_Idle2TP_REX_Hi after TP_RST
}

/*******************************************************
Description:
	Novatek touchscreen reset MCU (boot) function.

return:
	n.a.
*******************************************************/
void nvt_bootloader_reset(void)
{
	//---reset cmds to SWRST_N8_ADDR---
	nvt_write_addr(SWRST_N8_ADDR, 0x69);

	mdelay(5); //wait tBRST2FR after Bootload RST

	if (SPI_RD_FAST_ADDR) {
		/* disable SPI_RD_FAST */
		nvt_write_addr(SPI_RD_FAST_ADDR, 0x00);
	}
}

/*******************************************************
Description:
	Novatek touchscreen check FW reset state function.

return:
	Executive outcomes. 0---succeed. -1---failed.
*******************************************************/
int32_t nvt_check_fw_reset_state(RST_COMPLETE_STATE check_reset_state)
{
	uint8_t buf[8] = { 0 };
	int32_t ret = 0;
	int32_t retry = 0;
	int32_t retry_max = (check_reset_state == RESET_STATE_INIT) ? 10 : 50;

	//---set xdata index to EVENT BUF ADDR---
	nvt_set_page(ts->mmap->EVENT_BUF_ADDR | EVENT_MAP_RESET_COMPLETE);

	while (1) {
		//---read reset state---
		buf[0] = EVENT_MAP_RESET_COMPLETE;
		buf[1] = 0x00;
		CTP_SPI_READ(ts->client, buf, 6);

		if ((buf[1] >= check_reset_state) &&
		    (buf[1] <= RESET_STATE_MAX)) {
			ret = 0;
			break;
		}

		retry++;
		if (unlikely(retry > retry_max)) {
			NVT_ERR("error, retry=%d, buf[1]=0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X\n",
				retry, buf[1], buf[2], buf[3], buf[4], buf[5]);
			ret = -1;
			break;
		}

		usleep_range(10000, 10000);
	}

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen get novatek project id information
	function.

return:
	Executive outcomes. 0---success. -1---fail.
*******************************************************/
static int32_t nvt_read_pid(void)
{
	uint8_t buf[4] = { 0 };
	int32_t ret = 0;

	//---set xdata index to EVENT BUF ADDR---
	nvt_set_page(ts->mmap->EVENT_BUF_ADDR | EVENT_MAP_PROJECTID);

	//---read project id---
	buf[0] = EVENT_MAP_PROJECTID;
	buf[1] = 0x00;
	buf[2] = 0x00;
	CTP_SPI_READ(ts->client, buf, 3);

	//---set xdata index to EVENT BUF ADDR---
	nvt_set_page(ts->mmap->EVENT_BUF_ADDR);

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen get firmware related information
	function.

return:
	Executive outcomes. 0---success. -1---fail.
*******************************************************/
int32_t nvt_get_fw_info(void)
{
	uint8_t buf[64] = { 0 };
	uint32_t retry_count = 0;
	int32_t ret = 0;

info_retry:
	//---set xdata index to EVENT BUF ADDR---
	nvt_set_page(ts->mmap->EVENT_BUF_ADDR | EVENT_MAP_FWINFO);

	//---read fw info---
	buf[0] = EVENT_MAP_FWINFO;
	CTP_SPI_READ(ts->client, buf, 39);
	ts->fw_ver = buf[1];
	ts->x_num = buf[3];
	ts->y_num = buf[4];
	ts->abs_x_max = (uint16_t)((buf[5] << 8) | buf[6]);
	ts->abs_y_max = (uint16_t)((buf[7] << 8) | buf[8]);
	if (ts->pen_support) {
		ts->x_gang_num = buf[37];
		ts->y_gang_num = buf[38];
	}

	//---clear x_num, y_num if fw info is broken---
	if ((buf[1] + buf[2]) != 0xFF) {
		NVT_ERR("FW info is broken! fw_ver=0x%02X, ~fw_ver=0x%02X\n",
			buf[1], buf[2]);
		ts->fw_ver = 0;
		ts->x_num = 18;
		ts->y_num = 32;
		ts->abs_x_max = TOUCH_DEFAULT_MAX_WIDTH;
		ts->abs_y_max = TOUCH_DEFAULT_MAX_HEIGHT;

		if (retry_count < 3) {
			retry_count++;
			NVT_ERR("retry_count=%d\n", retry_count);
			goto info_retry;
		} else {
			NVT_ERR("Set default fw_ver=%d, x_num=%d, y_num=%d, "
				"abs_x_max=%d, abs_y_max=%d\n",
				ts->fw_ver, ts->x_num, ts->y_num, ts->abs_x_max,
				ts->abs_y_max);
			ret = -1;
		}
	} else {
		ret = 0;
	}

	NVT_LOG("fw_ver = 0x%02X, fw_type = 0x%02X, x_num=%d, y_num=%d\n",
		ts->fw_ver, buf[14], ts->x_num, ts->y_num);

	//---Get Novatek PID---
	nvt_read_pid();

	return ret;
}

static void release_pen_event(void)
{
	if (ts && ts->pen_input_dev) {
		input_report_abs(ts->pen_input_dev, ABS_X, 0);
		input_report_abs(ts->pen_input_dev, ABS_Y, 0);
		input_report_abs(ts->pen_input_dev, ABS_PRESSURE, 0);
		input_report_abs(ts->pen_input_dev, ABS_TILT_X, 0);
		input_report_abs(ts->pen_input_dev, ABS_TILT_Y, 0);
		input_report_abs(ts->pen_input_dev, ABS_DISTANCE, 0);
		input_report_key(ts->pen_input_dev, BTN_TOUCH, 0);
		input_report_key(ts->pen_input_dev, BTN_TOOL_PEN, 0);
		input_report_key(ts->pen_input_dev, BTN_STYLUS, 0);
		input_report_key(ts->pen_input_dev, BTN_STYLUS2, 0);
		input_sync(ts->pen_input_dev);
	}
}

/*******************************************************
Description:
	Novatek touchscreen parse device tree function.

return:
	n.a.
*******************************************************/
#ifdef CONFIG_OF
static int32_t nvt_parse_dt(struct device *dev)
{
	struct device_node *np = dev->of_node;
	int32_t ret = 0;

	ts->irq_gpio = of_get_named_gpio(np, "novatek,irq-gpio", 0);
	NVT_LOG("novatek,irq-gpio=%d\n", ts->irq_gpio);

	ts->pen_support = of_property_read_bool(np, "novatek,pen-support");
	NVT_LOG("novatek,pen-support=%d\n", ts->pen_support);

	ts->wgp_stylus = of_property_read_bool(np, "novatek,wgp-stylus");
	NVT_LOG("novatek,wgp-stylus=%d\n", ts->wgp_stylus);

	ret = of_property_read_u32(np, "novatek,swrst-n8-addr", &SWRST_N8_ADDR);
	if (ret) {
		NVT_ERR("error reading novatek,swrst-n8-addr. ret=%d\n", ret);
		return ret;
	} else {
		NVT_LOG("SWRST_N8_ADDR=0x%06X\n", SWRST_N8_ADDR);
	}

	ret = of_property_read_u32(np, "novatek,spi-rd-fast-addr",
				   &SPI_RD_FAST_ADDR);
	if (ret) {
		NVT_LOG("not support novatek,spi-rd-fast-addr\n");
		SPI_RD_FAST_ADDR = 0;
		ret = 0;
	} else {
		NVT_LOG("SPI_RD_FAST_ADDR=0x%06X\n", SPI_RD_FAST_ADDR);
	}

	ret = of_property_read_string(np, "firmware-name", &ts->fw_name);
	if (ret) {
		NVT_LOG("Unable to get touchscreen firmware name\n");
		ts->fw_name = DEFAULT_BOOT_UPDATE_FIRMWARE_NAME;
	}

	ret = of_property_read_u32(np, "spi-max-frequency", &ts->spi_max_freq);
	if (ret) {
		NVT_LOG("Unable to get spi freq\n");
		return ret;
	} else {
		NVT_LOG("spi-max-frequency: %u\n", ts->spi_max_freq);
	}

	return ret;
}
#else
static int32_t nvt_parse_dt(struct device *dev)
{
	ts->irq_gpio = NVTTOUCH_INT_PIN;
	return 0;
}
#endif

/*******************************************************
Description:
	Novatek touchscreen config and request gpio

return:
	Executive outcomes. 0---succeed. not 0---failed.
*******************************************************/
static int nvt_gpio_config(struct nvt_ts_data *ts)
{
	int32_t ret = 0;

	/* request INT-pin (Input) */
	if (gpio_is_valid(ts->irq_gpio)) {
		ret = gpio_request_one(ts->irq_gpio, GPIOF_IN, "NVT-int");
		if (ret) {
			NVT_ERR("Failed to request NVT-int GPIO\n");
		}
	}

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen deconfig gpio

return:
	n.a.
*******************************************************/
static void nvt_gpio_deconfig(struct nvt_ts_data *ts)
{
	if (gpio_is_valid(ts->irq_gpio))
		gpio_free(ts->irq_gpio);
}

static uint8_t nvt_fw_recovery(uint8_t *point_data)
{
	uint8_t i = 0;
	uint8_t detected = true;

	/* check pattern */
	for (i = 1; i < 7; i++) {
		if (point_data[i] != 0x77) {
			detected = false;
			break;
		}
	}

	return detected;
}

static uint8_t recovery_cnt = 0;
static uint8_t nvt_wdt_fw_recovery(uint8_t *point_data)
{
	uint32_t recovery_cnt_max = 10;
	uint8_t recovery_enable = false;
	uint8_t i = 0;

	recovery_cnt++;

	/* check pattern */
	for (i = 1; i < 7; i++) {
		if ((point_data[i] != 0xFD) && (point_data[i] != 0xFE)) {
			recovery_cnt = 0;
			break;
		}
	}

	if (recovery_cnt > recovery_cnt_max) {
		recovery_enable = true;
		recovery_cnt = 0;
	}

	return recovery_enable;
}

#define PEN_DATA_LEN 14
#define FW_HISTORY_SIZE 128
static uint32_t nvt_dump_fw_history(void)
{
	int32_t ret = 0;
	uint8_t buf[FW_HISTORY_SIZE + 1 + DUMMY_BYTES] = { 0 };
	int32_t i = 0;
	char *tmp_dump = NULL;
	int32_t line_cnt = 0;

	if (ts->mmap->FW_HISTORY_ADDR == 0) {
		NVT_ERR("FW_HISTORY_ADDR not available!\n");
		ret = -1;
		goto exit_nvt_dump_fw_history;
	}
	nvt_set_page(ts->mmap->FW_HISTORY_ADDR);
	buf[0] = ts->mmap->FW_HISTORY_ADDR & 0xFF;
	CTP_SPI_READ(ts->client, buf, FW_HISTORY_SIZE + 1);
	if (ret) {
		NVT_ERR("CTP_SPI_READ failed.(%d)\n", ret);
		ret = -1;
		goto exit_nvt_dump_fw_history;
	}

	tmp_dump = (char *)kzalloc(FW_HISTORY_SIZE * 4, GFP_KERNEL);
	for (i = 0; i < FW_HISTORY_SIZE; i++) {
		sprintf(tmp_dump + i * 3 + line_cnt, "%02X ", buf[1 + i]);
		if ((i + 1) % 16 == 0) {
			sprintf(tmp_dump + i * 3 + line_cnt + 3, "%c", '\n');
			line_cnt++;
		}
	}
	NVT_LOG("%s", tmp_dump);

exit_nvt_dump_fw_history:
	if (tmp_dump) {
		kfree(tmp_dump);
		tmp_dump = NULL;
	}
	nvt_set_page(ts->mmap->EVENT_BUF_ADDR);

	return ret;
}

#define POINT_DATA_LEN 65
/*******************************************************
Description:
	Novatek touchscreen work function.

return:
	n.a.
*******************************************************/
static irqreturn_t nvt_ts_work_func(int irq, void *data)
{
	uint32_t position = 0;
	uint32_t input_x = 0;
	uint32_t input_y = 0;
	uint32_t input_w = 0;
	uint32_t input_p = 0;
	uint8_t input_id = 0;
	uint8_t press_id[TOUCH_MAX_FINGER_NUM] = { 0 };
	int32_t i = 0;
	int32_t finger_cnt = 0;
	uint8_t pen_format_id = 0;
	uint32_t pen_x = 0;
	uint32_t pen_y = 0;
	uint32_t pen_pressure = 0;
	uint32_t pen_distance = 0;
	int8_t pen_tilt_x = 0;
	int8_t pen_tilt_y = 0;
	uint32_t pen_btn1 = 0;
	uint32_t pen_btn2 = 0;
	uint32_t pen_battery = 0;
	uint8_t cmd;
	uint8_t *buf;

	static struct task_struct *touch_task = NULL;

	if (touch_task == NULL) {
		touch_task = current;
		sched_set_fifo(touch_task);
	}

	if (ts->dev_pm_suspend) {
		if (!wait_for_completion_timeout(&ts->dev_pm_suspend_completion,
						 msecs_to_jiffies(500))) {
			NVT_ERR("system(spi) can't finished resuming procedure, skip it\n");
			return IRQ_HANDLED;
		}
	}

	mutex_lock(&ts->xbuf_lock);
	cmd = SPI_READ_MASK(0);
	if (nvt_spi_read(ts->client, &cmd,
			 ts->pen_support ? POINT_DATA_LEN + PEN_DATA_LEN + 1 :
					   POINT_DATA_LEN + 1) < 0) {
		mutex_unlock(&ts->xbuf_lock);
		NVT_ERR("CTP_SPI_READ failed\n");
		return IRQ_HANDLED;
	}
	mutex_unlock(&ts->xbuf_lock);

	/* Data starts at ts->rbuf + 2, buf[1] maps to ts->rbuf[2] */
	buf = ts->rbuf + 1;

	/* ESD protect by WDT */
	if (nvt_wdt_fw_recovery(buf)) {
		NVT_ERR("Recover for fw reset, %02X\n", buf[1]);
		if (buf[1] == 0xFD) {
			NVT_ERR("Dump FW history:\n");
			nvt_dump_fw_history();
		}
		nvt_update_firmware(ts->fw_name);
		return IRQ_HANDLED;
	}

	if (nvt_fw_recovery(buf))
		return IRQ_HANDLED;

	finger_cnt = 0;

	for (i = 0; i < ts->max_touch_num; i++) {
		position = 1 + 6 * i;
		input_id = (uint8_t)(buf[position + 0] >> 3);
		if ((input_id == 0) || (input_id > ts->max_touch_num))
			continue;

		if (((buf[position] & 0x07) == 0x01) ||
		    ((buf[position] & 0x07) ==
		     0x02)) { //finger down (enter & moving)
			input_x = (uint32_t)(buf[position + 1] << 4) +
				  (uint32_t)(buf[position + 3] >> 4);
			input_y = (uint32_t)(buf[position + 2] << 4) +
				  (uint32_t)(buf[position + 3] & 0x0F);
			if ((input_x > ts->abs_x_max) ||
			    (input_y > ts->abs_y_max))
				continue;
			input_w = (uint32_t)(buf[position + 4]);
			if (input_w == 0)
				input_w = 1;
			if (i < 2) {
				input_p = (uint32_t)(buf[position + 5]) +
					  (uint32_t)(buf[i + 63] << 8);
				if (input_p > TOUCH_FORCE_NUM)
					input_p = TOUCH_FORCE_NUM;
			} else {
				input_p = (uint32_t)(buf[position + 5]);
			}
			if (input_p == 0)
				input_p = 1;

			press_id[input_id - 1] = 1;
			input_mt_slot(ts->input_dev, input_id - 1);
			input_mt_report_slot_state(ts->input_dev,
						   MT_TOOL_FINGER, true);

			input_report_abs(ts->input_dev, ABS_MT_POSITION_X,
					 input_x);
			input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,
					 input_y);
			input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR,
					 input_w);
			input_report_abs(ts->input_dev, ABS_MT_PRESSURE,
					 input_p);

			finger_cnt++;
		}
	}

	for (i = 0; i < ts->max_touch_num; i++) {
		if (press_id[i] != 1) {
			input_mt_slot(ts->input_dev, i);
			input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0);
			input_report_abs(ts->input_dev, ABS_MT_PRESSURE, 0);
			input_mt_report_slot_state(ts->input_dev,
						   MT_TOOL_FINGER, false);
		}
	}
	input_report_key(ts->input_dev, BTN_TOUCH, (finger_cnt > 0));

	input_sync(ts->input_dev);

	if (!ts->pen_support)
		return IRQ_HANDLED;

	// parse and handle pen report
	pen_format_id = buf[66];
	if (pen_format_id != 0xFF) {
		if (pen_format_id == 0x01) {
			// report pen data
			pen_x = (uint32_t)(buf[67] << 8) + (uint32_t)(buf[68]);
			pen_y = (uint32_t)(buf[69] << 8) + (uint32_t)(buf[70]);
			if (pen_x >= ts->abs_x_max * 8 - 1) {
				pen_x -= 1;
			}
			if (pen_y >= ts->abs_y_max * 8 - 1) {
				pen_y -= 1;
			}
			pen_pressure =
				(uint32_t)(buf[71] << 8) + (uint32_t)(buf[72]);
			pen_tilt_x = (int8_t)buf[73];
			pen_tilt_y = (int8_t)buf[74];
			pen_distance =
				(uint32_t)(buf[75] << 8) + (uint32_t)(buf[76]);
			pen_btn1 = (uint32_t)(buf[77] & 0x01);
			pen_btn2 = (uint32_t)((buf[77] >> 1) & 0x01);
			pen_battery = (uint32_t)buf[78];

			input_report_abs(ts->pen_input_dev, ABS_X, pen_x);
			input_report_abs(ts->pen_input_dev, ABS_Y, pen_y);
			input_report_abs(ts->pen_input_dev, ABS_PRESSURE,
					 pen_pressure);
			input_report_key(ts->pen_input_dev, BTN_TOUCH,
					 !!pen_pressure);
			input_report_abs(ts->pen_input_dev, ABS_TILT_X,
					 pen_tilt_x);
			input_report_abs(ts->pen_input_dev, ABS_TILT_Y,
					 pen_tilt_y);
			input_report_abs(ts->pen_input_dev, ABS_DISTANCE,
					 pen_distance);
			input_report_key(ts->pen_input_dev, BTN_TOOL_PEN,
					 !!pen_distance || !!pen_pressure);
			input_report_key(ts->pen_input_dev, BTN_STYLUS,
					 pen_btn1);
			input_report_key(ts->pen_input_dev, BTN_STYLUS2,
					 pen_btn2);
			input_sync(ts->pen_input_dev);
		} else if (pen_format_id == 0xF0) {
			// report Pen ID
		} else {
			NVT_ERR("Unknown pen format id!\n");
		}
	} else {
		release_pen_event();
	}
	return IRQ_HANDLED;
}

/*******************************************************
Description:
	Novatek touchscreen check chip version trim function.

return:
	Executive outcomes. 0---NVT IC. -1---not NVT IC.
*******************************************************/
static int8_t nvt_ts_check_chip_ver_trim(uint32_t chip_ver_trim_addr)
{
	ts->mmap = &NT36523_memory_map;
	ts->carrier_system = NT36523_hw_info.carrier_system;
	ts->hw_crc = NT36523_hw_info.hw_crc;
	return 0;
}

static int32_t disable_pen_input_device(bool disable)
{
	uint8_t buf[8] = { 0 };
	int32_t ret = 0;

	NVT_LOG("++\n");
	if (!bTouchIsAwake || !ts) {
		NVT_LOG("touch suspend, stop set pen state %s",
			disable ? "DISABLE" : "ENABLE");
		goto nvt_set_pen_enable_out;
	}

	msleep(35);

	//---set xdata index to EVENT BUF ADDR---
	ret = nvt_set_page(ts->mmap->EVENT_BUF_ADDR | EVENT_MAP_HOST_CMD);
	if (ret < 0) {
		NVT_ERR("Set event buffer index fail!\n");
		goto nvt_set_pen_enable_out;
	}

	buf[0] = EVENT_MAP_HOST_CMD;
	buf[1] = 0x7B;
	buf[2] = !!disable;
	ret = CTP_SPI_WRITE(ts->client, buf, 3);
	if (ret < 0) {
		NVT_ERR("set pen %s failed!\n", disable ? "DISABLE" : "ENABLE");
		goto nvt_set_pen_enable_out;
	}
	NVT_LOG("%s pen input device\n", disable ? "DISABLE" : "ENABLE");

nvt_set_pen_enable_out:
	NVT_LOG("--\n");
	return ret;
}

static void nvt_suspend_work(struct work_struct *work)
{
	struct nvt_ts_data *ts_core =
		container_of(work, struct nvt_ts_data, suspend_work);
	nvt_ts_suspend(&ts_core->client->dev);
}

static void nvt_resume_work(struct work_struct *work)
{
	struct nvt_ts_data *ts_core =
		container_of(work, struct nvt_ts_data, resume_work);
	nvt_ts_resume(&ts_core->client->dev);
}

/*******************************************************
Description:
	Novatek touchscreen driver probe function.

return:
	Executive outcomes. 0---succeed. negative---failed
*******************************************************/
static int32_t nvt_ts_probe(struct spi_device *client)
{
	int32_t ret = 0;

	NVT_LOG("probe start\n");

	ts = kzalloc(sizeof(struct nvt_ts_data), GFP_KERNEL);
	if (ts == NULL) {
		NVT_ERR("failed to allocated memory for nvt ts data\n");
		return -ENOMEM;
	}

	ts->xbuf = (uint8_t *)kzalloc((NVT_TRANSFER_LEN + 1 + DUMMY_BYTES),
				      GFP_KERNEL);
	if (ts->xbuf == NULL) {
		NVT_ERR("kzalloc for xbuf failed!\n");
		ret = -ENOMEM;
		goto err_malloc_xbuf;
	}

	ts->rbuf = (uint8_t *)kzalloc(NVT_READ_LEN, GFP_KERNEL);
	if (ts->rbuf == NULL) {
		NVT_ERR("kzalloc for rbuf failed!\n");
		ret = -ENOMEM;
		goto err_malloc_rbuf;
	}

	ts->client = client;
	spi_set_drvdata(client, ts);

	//---prepare for spi parameter---
	if (ts->client->controller->flags & SPI_CONTROLLER_HALF_DUPLEX) {
		NVT_ERR("Full duplex not supported by master\n");
		ret = -EIO;
		goto err_ckeck_full_duplex;
	}
	ts->client->bits_per_word = 8;
	ts->client->mode = SPI_MODE_0;

	ret = spi_setup(ts->client);
	if (ret < 0) {
		NVT_ERR("Failed to perform SPI setup\n");
		goto err_spi_setup;
	}

	NVT_LOG("mode=%d, max_speed_hz=%d\n", ts->client->mode,
		ts->client->max_speed_hz);

	//---parse dts---
	ret = nvt_parse_dt(&client->dev);
	if (ret) {
		NVT_ERR("parse dt error\n");
		goto err_spi_setup;
	}

	//---request and config GPIOs---
	ret = nvt_gpio_config(ts);
	if (ret) {
		NVT_ERR("gpio config error!\n");
		goto err_gpio_config_failed;
	}

	mutex_init(&ts->xbuf_lock);

	//---eng reset before TP_RESX high
	nvt_eng_reset();

	// need 10ms delay after POR(power on reset)
	msleep(10);

	//---check chip version trim---
	ret = nvt_ts_check_chip_ver_trim(CHIP_VER_TRIM_ADDR);
	if (ret) {
		NVT_LOG("try to check from old chip ver trim address\n");
		ret = nvt_ts_check_chip_ver_trim(CHIP_VER_TRIM_OLD_ADDR);
		if (ret) {
			NVT_ERR("chip is not identified\n");
			ret = -EINVAL;
			goto err_chipvertrim_failed;
		}
	}

	ts->abs_x_max = TOUCH_DEFAULT_MAX_WIDTH;
	ts->abs_y_max = TOUCH_DEFAULT_MAX_HEIGHT;

	//---allocate input device---
	ts->input_dev = input_allocate_device();
	if (ts->input_dev == NULL) {
		NVT_ERR("allocate input device failed\n");
		ret = -ENOMEM;
		goto err_input_dev_alloc_failed;
	}

	ts->max_touch_num = TOUCH_MAX_FINGER_NUM;

	ts->int_trigger_type = INT_TRIGGER_TYPE;

	//---set input device info.---
	ts->input_dev->evbit[0] = BIT_MASK(EV_SYN) | BIT_MASK(EV_KEY) |
				  BIT_MASK(EV_ABS);
	ts->input_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
	ts->input_dev->propbit[0] = BIT(INPUT_PROP_DIRECT);

	ts->db_wakeup = 0;
	ts->fw_ver = 0;
	ts->x_num = 32;
	ts->y_num = 50;
	ts->x_gang_num = 4;
	ts->y_gang_num = 6;
	ts->abs_x_max = TOUCH_DEFAULT_MAX_WIDTH;
	ts->abs_y_max = TOUCH_DEFAULT_MAX_HEIGHT;
	NVT_LOG("Set default fw_ver=%d, x_num=%d, y_num=%d, "
		"abs_x_max=%d, abs_y_max=%d\n",
		ts->fw_ver, ts->x_num, ts->y_num, ts->abs_x_max, ts->abs_y_max);

	input_mt_init_slots(ts->input_dev, ts->max_touch_num, 0);

	input_set_abs_params(ts->input_dev, ABS_MT_PRESSURE, 0, TOUCH_FORCE_NUM,
			     0, 0); //pressure = TOUCH_FORCE_NUM

#if TOUCH_MAX_FINGER_NUM > 1
	input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0,
			     0); //area = 255

	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X, 0,
			     ts->abs_x_max - 1, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y, 0,
			     ts->abs_y_max - 1, 0, 0);
#endif //TOUCH_MAX_FINGER_NUM > 1

	sprintf(ts->phys, "input/ts");
	ts->input_dev->name = NVT_TS_NAME;
	ts->input_dev->phys = ts->phys;
	ts->input_dev->id.bustype = BUS_SPI;

	//---register input device---
	ret = input_register_device(ts->input_dev);
	if (ret) {
		NVT_ERR("register input device (%s) failed. ret=%d\n",
			ts->input_dev->name, ret);
		goto err_input_register_device_failed;
	}

	if (ts->pen_support) {
		//---allocate pen input device---
		ts->pen_input_dev = input_allocate_device();
		if (ts->pen_input_dev == NULL) {
			NVT_ERR("allocate pen input device failed\n");
			ret = -ENOMEM;
			goto err_pen_input_dev_alloc_failed;
		}

		//---set pen input device info.---
		ts->pen_input_dev->evbit[0] =
			BIT_MASK(EV_SYN) | BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
		ts->pen_input_dev->keybit[BIT_WORD(BTN_TOUCH)] =
			BIT_MASK(BTN_TOUCH);
		ts->pen_input_dev->keybit[BIT_WORD(BTN_TOOL_PEN)] |=
			BIT_MASK(BTN_TOOL_PEN);
		//ts->pen_input_dev->keybit[BIT_WORD(BTN_TOOL_RUBBER)] |= BIT_MASK(BTN_TOOL_RUBBER);
		ts->pen_input_dev->keybit[BIT_WORD(BTN_STYLUS)] |=
			BIT_MASK(BTN_STYLUS);
		ts->pen_input_dev->keybit[BIT_WORD(BTN_STYLUS2)] |=
			BIT_MASK(BTN_STYLUS2);
		ts->pen_input_dev->propbit[0] = BIT(INPUT_PROP_DIRECT);

		int x_max, y_max;

		if (ts->wgp_stylus) {
			x_max = ts->abs_x_max * 8 - 1;
			y_max = ts->abs_y_max * 8 - 1;
		} else {
			x_max = ts->abs_x_max - 1;
			y_max = ts->abs_y_max - 1;
		}

		input_set_abs_params(ts->pen_input_dev, ABS_X, 0, x_max, 0, 0);
		input_set_abs_params(ts->pen_input_dev, ABS_Y, 0, y_max, 0, 0);
		input_abs_set_res(ts->pen_input_dev, ABS_X,
				  x_max / PANEL_DEFAULT_WIDTH_MM);
		input_abs_set_res(ts->pen_input_dev, ABS_Y,
				  y_max / PANEL_DEFAULT_HEIGHT_MM);

		input_set_abs_params(ts->pen_input_dev, ABS_PRESSURE, 0,
				     PEN_PRESSURE_MAX, 0, 0);
		input_set_abs_params(ts->pen_input_dev, ABS_DISTANCE, 0,
				     PEN_DISTANCE_MAX, 0, 0);
		input_set_abs_params(ts->pen_input_dev, ABS_TILT_X,
				     PEN_TILT_MIN, PEN_TILT_MAX, 0, 0);
		input_set_abs_params(ts->pen_input_dev, ABS_TILT_Y,
				     PEN_TILT_MIN, PEN_TILT_MAX, 0, 0);

		sprintf(ts->pen_phys, "input/pen");
		ts->pen_input_dev->name = NVT_PEN_NAME;
		ts->pen_input_dev->phys = ts->pen_phys;
		ts->pen_input_dev->id.bustype = BUS_SPI;

		//---register pen input device---
		ret = input_register_device(ts->pen_input_dev);
		if (ret) {
			NVT_ERR("register pen input device (%s) failed. ret=%d\n",
				ts->pen_input_dev->name, ret);
			goto err_pen_input_register_device_failed;
		}
	} /* if (ts->pen_support) */

	//---set int-pin & request irq---
	client->irq = gpio_to_irq(ts->irq_gpio);
	if (client->irq) {
		NVT_LOG("int_trigger_type=%d\n", ts->int_trigger_type);
		ts->irq_enabled = true;
		ret = request_threaded_irq(client->irq, NULL, nvt_ts_work_func,
					   ts->int_trigger_type | IRQF_ONESHOT,
					   NVT_SPI_NAME, ts);
		if (ret != 0) {
			NVT_ERR("request irq failed. ret=%d\n", ret);
			goto err_int_request_failed;
		} else {
			nvt_irq_enable(false);
			NVT_LOG("request irq %d succeed\n", client->irq);
		}
	}

	pm_stay_awake(&client->dev);

	ts->ic_state = NVT_IC_INIT;
	ts->dev_pm_suspend = false;
	ts->gesture_command_delayed = -1;
	init_completion(&ts->dev_pm_suspend_completion);

	ret = nvt_update_firmware(ts->fw_name);
	if (ret)
		NVT_ERR("download firmware failed\n");

	ts->event_wq =
		alloc_workqueue("nvt-event-queue",
				WQ_UNBOUND | WQ_HIGHPRI | WQ_CPU_INTENSIVE, 1);
	if (!ts->event_wq) {
		NVT_ERR("Can not create work thread for suspend/resume!!");
		ret = -ENOMEM;
		goto err_alloc_work_thread_failed;
	}
	INIT_WORK(&ts->resume_work, nvt_resume_work);
	INIT_WORK(&ts->suspend_work, nvt_suspend_work);

#ifdef CONFIG_DRM
	ts->drm_notif.notifier_call = nvt_drm_notifier_callback;
	ret = mi_drm_register_client(&ts->drm_notif);
	if (ret) {
		NVT_ERR("register drm_notifier failed. ret=%d\n", ret);
		goto err_register_drm_notif_failed;
	}
#endif

	bTouchIsAwake = 1;
	disable_pen_input_device(false);
	NVT_LOG("end\n");

	nvt_irq_enable(true);

	return 0;

#ifdef CONFIG_DRM
	if (mi_drm_unregister_client(&ts->drm_notif))
		NVT_ERR("Error occurred while unregistering drm_notifier.\n");
err_register_drm_notif_failed:
#endif

err_alloc_work_thread_failed:
	free_irq(client->irq, ts);
err_int_request_failed:
	if (ts->pen_support) {
		input_unregister_device(ts->pen_input_dev);
		ts->pen_input_dev = NULL;
	}
err_pen_input_register_device_failed:
	if (ts->pen_support) {
		if (ts->pen_input_dev) {
			input_free_device(ts->pen_input_dev);
			ts->pen_input_dev = NULL;
		}
	}
err_pen_input_dev_alloc_failed:
	input_unregister_device(ts->input_dev);
	ts->input_dev = NULL;
err_input_register_device_failed:
	if (ts->input_dev) {
		input_free_device(ts->input_dev);
		ts->input_dev = NULL;
	}
err_input_dev_alloc_failed:
err_chipvertrim_failed:
	mutex_destroy(&ts->xbuf_lock);
	nvt_gpio_deconfig(ts);
err_gpio_config_failed:
err_spi_setup:
err_ckeck_full_duplex:
	spi_set_drvdata(client, NULL);
	if (ts->rbuf) {
		kfree(ts->rbuf);
		ts->rbuf = NULL;
	}
err_malloc_rbuf:
	if (ts->xbuf) {
		kfree(ts->xbuf);
		ts->xbuf = NULL;
	}
err_malloc_xbuf:
	if (ts) {
		kfree(ts);
		ts = NULL;
	}
	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen driver release function.

return:
	Executive outcomes. 0---succeed.
*******************************************************/
static void nvt_ts_remove(struct spi_device *client)
{
	NVT_LOG("Removing driver...\n");

#ifdef CONFIG_DRM
	if (mi_drm_unregister_client(&ts->drm_notif))
		NVT_ERR("Error occurred while unregistering drm_notifier.\n");
#endif

	nvt_irq_enable(false);
	free_irq(client->irq, ts);

	mutex_destroy(&ts->xbuf_lock);

	nvt_gpio_deconfig(ts);

	if (ts->pen_support) {
		if (ts->pen_input_dev) {
			input_unregister_device(ts->pen_input_dev);
			ts->pen_input_dev = NULL;
		}
	}

	if (ts->input_dev) {
		input_unregister_device(ts->input_dev);
		ts->input_dev = NULL;
	}

	spi_set_drvdata(client, NULL);

	if (ts) {
		kfree(ts);
		ts = NULL;
	}
}

static void nvt_ts_shutdown(struct spi_device *client)
{
	NVT_LOG("Shutdown driver...\n");

	nvt_irq_enable(false);

#ifdef CONFIG_DRM
	if (mi_drm_unregister_client(&ts->drm_notif))
		NVT_ERR("Error occurred while unregistering drm_notifier.\n");
#endif

	destroy_workqueue(ts->event_wq);
}

/*******************************************************
Description:
	Novatek touchscreen driver suspend function.

return:
	Executive outcomes. 0---succeed.
*******************************************************/
static int32_t nvt_ts_suspend(struct device *dev)
{
	uint8_t buf[4] = { 0 };
	uint32_t i = 0;

	if (!bTouchIsAwake) {
		NVT_LOG("Touch is already suspend\n");
		return 0;
	}

	pm_stay_awake(dev);
	ts->ic_state = NVT_IC_SUSPEND_IN;

	if (!ts->db_wakeup) {
		if (!ts->irq_enabled)
			NVT_LOG("IRQ already disabled\n");
		else
			nvt_irq_enable(false);
	}

	NVT_LOG("suspend start\n");

	bTouchIsAwake = 0;

	if (ts->db_wakeup) {
		/*---write command to enter "wakeup gesture mode"---*/
		/*DoubleClick wakeup CMD was sent by display to meet timing*/
		/*
		buf[0] = EVENT_MAP_HOST_CMD;
		buf[1] = 0x13;
		CTP_SPI_WRITE(ts->client, buf, 2);
		*/
		enable_irq_wake(ts->client->irq);

		NVT_LOG("Enabled touch wakeup gesture\n");
	} else {
		/*---write command to enter "deep sleep mode"---*/
		buf[0] = EVENT_MAP_HOST_CMD;
		buf[1] = 0x11;
		CTP_SPI_WRITE(ts->client, buf, 2);
	}

	/* release all touches */
	for (i = 0; i < ts->max_touch_num; i++) {
		input_mt_slot(ts->input_dev, i);
		input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0);
		input_report_abs(ts->input_dev, ABS_MT_PRESSURE, 0);
		input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, 0);
	}
	input_report_key(ts->input_dev, BTN_TOUCH, 0);
	input_sync(ts->input_dev);

	msleep(50);
	/* release pen event */
	release_pen_event();
	if (likely(ts->ic_state == NVT_IC_SUSPEND_IN))
		ts->ic_state = NVT_IC_SUSPEND_OUT;
	else
		NVT_ERR("IC state may error,caused by suspend/resume flow, please CHECK!!");
	pm_relax(dev);
	NVT_LOG("end\n");

	return 0;
}

/*******************************************************
Description:
	Novatek touchscreen driver resume function.

return:
	Executive outcomes. 0---succeed.
*******************************************************/
static int32_t nvt_ts_resume(struct device *dev)
{
	if (bTouchIsAwake) {
		NVT_LOG("Touch is already resume\n");
		return 0;
	}

	if (ts->dev_pm_suspend)
		pm_stay_awake(dev);

	NVT_LOG("resume start\n");
	ts->ic_state = NVT_IC_RESUME_IN;

	// please make sure display reset(RESX) sequence and mipi dsi cmds sent before this
	if (nvt_update_firmware(ts->fw_name))
		NVT_ERR("download firmware failed\n");

	nvt_check_fw_reset_state(RESET_STATE_REK);

	if (!ts->db_wakeup && !ts->irq_enabled)
		nvt_irq_enable(true);

	bTouchIsAwake = 1;

	disable_pen_input_device(false);
	if (likely(ts->ic_state == NVT_IC_RESUME_IN)) {
		ts->ic_state = NVT_IC_RESUME_OUT;
	} else {
		NVT_ERR("IC state may error,caused by suspend/resume flow, please CHECK!!");
	}
	if (ts->gesture_command_delayed >= 0) {
		ts->db_wakeup = ts->gesture_command_delayed;
		ts->gesture_command_delayed = -1;
		NVT_LOG("execute delayed command, set double click wakeup %d\n",
			ts->db_wakeup);
	}

	if (ts->dev_pm_suspend)
		pm_relax(dev);
	NVT_LOG("end\n");

	return 0;
}

#ifdef CONFIG_DRM
static int nvt_drm_notifier_callback(struct notifier_block *self,
				     unsigned long event, void *data)
{
	int blank = *(enum drm_notifier_data *)data;
	struct nvt_ts_data *ts_data =
		container_of(self, struct nvt_ts_data, drm_notif);

	if (data && ts_data) {
		if (event == MI_DRM_EARLY_EVENT_BLANK) {
			if (blank == MI_DRM_BLANK_POWERDOWN) {
				NVT_LOG("event=%lu, *blank=%d\n", event, blank);
				flush_workqueue(ts_data->event_wq);
				queue_work(ts_data->event_wq,
					   &ts_data->suspend_work);
			}
		} else if (event == MI_DRM_EVENT_BLANK) {
			if (blank == MI_DRM_BLANK_UNBLANK) {
				NVT_LOG("event=%lu, *blank=%d\n", event, blank);
				flush_workqueue(ts_data->event_wq);
				queue_work(ts_data->event_wq,
					   &ts_data->resume_work);
			}
		}
	}

	return 0;
}
#endif

static int nvt_pm_suspend(struct device *dev)
{
	if (device_may_wakeup(dev) && ts->db_wakeup) {
		NVT_LOG("enable touch irq wake\n");
		enable_irq_wake(ts->client->irq);
	}
	ts->dev_pm_suspend = true;
	reinit_completion(&ts->dev_pm_suspend_completion);

	return 0;
}

static int nvt_pm_resume(struct device *dev)
{
	if (device_may_wakeup(dev) && ts->db_wakeup) {
		NVT_LOG("disable touch irq wake\n");
		disable_irq_wake(ts->client->irq);
	}
	ts->dev_pm_suspend = false;
	complete(&ts->dev_pm_suspend_completion);

	return 0;
}

static const struct dev_pm_ops nvt_dev_pm_ops = {
	.suspend = nvt_pm_suspend,
	.resume = nvt_pm_resume,
};

static const struct spi_device_id nvt_ts_id[] = { { NVT_SPI_NAME, 0 }, {} };
MODULE_DEVICE_TABLE(spi, nvt_ts_id);

#ifdef CONFIG_OF
static struct of_device_id nvt_match_table[] = {
	{
		.compatible = "novatek,NVT-ts-spi",
	},
	{},
};
MODULE_DEVICE_TABLE(of, nvt_match_table);
#endif

static struct spi_driver nvt_spi_driver = {
	.probe		= nvt_ts_probe,
	.remove		= nvt_ts_remove,
	.shutdown	= nvt_ts_shutdown,
	.id_table	= nvt_ts_id,
	.driver = {
		.name	= NVT_SPI_NAME,
		.owner	= THIS_MODULE,
#ifdef CONFIG_PM
		.pm = &nvt_dev_pm_ops,
#endif
#ifdef CONFIG_OF
		.of_match_table = nvt_match_table,
#endif
	},
};

module_spi_driver(nvt_spi_driver);

MODULE_DESCRIPTION("Novatek Touchscreen Driver");
MODULE_LICENSE("GPL");
