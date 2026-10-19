/*
 * Copyright (C) 2010 - 2018 Novatek, Inc.
 * Copyright (C) 2021 XiaoMi, Inc.
 *
 * $Revision: 69262 $
 * $Date: 2020-09-23 15:07:14 +0800 (週三, 23 九月 2020) $
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
#ifndef _LINUX_NVT_TOUCH_H
#define _LINUX_NVT_TOUCH_H

#include <linux/delay.h>
#include <linux/input.h>
#include <linux/of.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>

#include "nt36xxx_mem_map.h"

#define PINCTRL_STATE_ACTIVE "pmx_ts_active"
#define PINCTRL_STATE_SUSPEND "pmx_ts_suspend"

//---INT trigger mode---
#define INT_TRIGGER_TYPE IRQ_TYPE_EDGE_RISING

//---SPI driver info.---
#define NVT_SPI_NAME "NVT-ts-spi"

#define NVT_LOG(fmt, args...) \
	pr_info("[%s] %s %d: " fmt, NVT_SPI_NAME, __func__, __LINE__, ##args)
#define NVT_ERR(fmt, args...) \
	pr_err("[%s] %s %d: " fmt, NVT_SPI_NAME, __func__, __LINE__, ##args)

//---Input device info.---
#define NVT_TS_NAME "NVTCapacitiveTouchScreen"
#define NVT_PEN_NAME "NVTCapacitivePen"

//---Touch info.---
#define TOUCH_DEFAULT_MAX_WIDTH 1600
#define TOUCH_DEFAULT_MAX_HEIGHT 2560
#define TOUCH_MAX_FINGER_NUM 10
#define TOUCH_FORCE_NUM 1000

//---for Pen---
#define PEN_PRESSURE_MAX (4095)
#define PEN_DISTANCE_MAX (1)
#define PEN_TILT_MIN (-60)
#define PEN_TILT_MAX (60)

//---for pen resolution---
#define PANEL_DEFAULT_WIDTH_MM 148 // 148mm
#define PANEL_DEFAULT_HEIGHT_MM 237 // 237mm

//---Firmware path---
#define DEFAULT_BOOT_UPDATE_FIRMWARE_NAME "novatek/nt36523.bin"

enum nvt_ic_state {
	NVT_IC_SUSPEND_IN,
	NVT_IC_SUSPEND_OUT,
	NVT_IC_RESUME_IN,
	NVT_IC_RESUME_OUT,
	NVT_IC_INIT,
};

struct nvt_ts_data {
	struct spi_device *client;
	struct input_dev *input_dev;
	struct input_dev *pen_input_dev;
	struct mutex xbuf_lock;
	struct pinctrl *ts_pinctrl;
	struct pinctrl_state *pinctrl_state_active;
	struct pinctrl_state *pinctrl_state_suspend;
	struct workqueue_struct *event_wq;
	struct work_struct suspend_work;
	struct work_struct resume_work;
	struct completion dev_pm_suspend_completion;
#ifdef CONFIG_DRM
	struct notifier_block drm_notif;
#endif
	const struct nvt_ts_mem_map *mmap;
	const char *fw_name;
	int ic_state;
	int db_wakeup;
	int gesture_command_delayed;
	bool irq_enabled;
	bool pen_support;
	bool wgp_stylus;
	bool dev_pm_suspend;
	int8_t phys[32];
	int8_t pen_phys[32];
	uint8_t fw_ver;
	uint8_t x_num;
	uint8_t y_num;
	uint8_t max_touch_num;
	uint8_t carrier_system;
	uint8_t hw_crc;
	uint8_t *rbuf;
	uint8_t *xbuf;
	uint8_t x_gang_num;
	uint8_t y_gang_num;
	uint16_t abs_x_max;
	uint16_t abs_y_max;
	int32_t irq_gpio;
	int32_t reset_gpio;
	uint32_t irq_flags;
	uint32_t reset_flags;
	uint32_t spi_max_freq;
	uint32_t int_trigger_type;
};

typedef enum {
	RESET_STATE_INIT = 0xA0, // IC reset
	RESET_STATE_REK, // ReK baseline
	RESET_STATE_REK_FINISH, // baseline is ready
	RESET_STATE_NORMAL_RUN, // normal run
	RESET_STATE_MAX = 0xAF
} RST_COMPLETE_STATE;

//---Event map---
#define EVENT_MAP_HOST_CMD 0x50
#define EVENT_MAP_HANDSHAKING_or_SUB_CMD_BYTE 0x51
#define EVENT_MAP_RESET_COMPLETE 0x60
#define EVENT_MAP_FWINFO 0x78
#define EVENT_MAP_PROJECTID 0x9A

//---SPI READ/WRITE---
#define SPI_WRITE_MASK(a) (a | 0x80)
#define SPI_READ_MASK(a) (a & 0x7F)

#define DUMMY_BYTES (1)
#define NVT_TRANSFER_LEN (63 * 1024)
#define NVT_READ_LEN (2 * 1024)

//---extern structures---
extern struct nvt_ts_data *ts;

//---extern functions---
int32_t CTP_SPI_READ(struct spi_device *client, uint8_t *buf, uint16_t len);
int32_t CTP_SPI_WRITE(struct spi_device *client, uint8_t *buf, uint16_t len);
void nvt_bootloader_reset(void);
void nvt_eng_reset(void);
void nvt_boot_ready(void);
void nvt_bld_crc_enable(void);
void nvt_fw_crc_enable(void);
void nvt_tx_auto_copy_mode(void);
int32_t nvt_update_firmware(const char *firmware_name);
int32_t nvt_check_fw_reset_state(RST_COMPLETE_STATE check_reset_state);
int32_t nvt_get_fw_info(void);
int32_t nvt_check_spi_dma_tx_info(void);
int32_t nvt_set_page(uint32_t addr);
int32_t nvt_write_addr(uint32_t addr, uint8_t data);

#endif /* _LINUX_NVT_TOUCH_H */