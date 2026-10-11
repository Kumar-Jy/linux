/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __IRIS_POWER_H__
#define __IRIS_POWER_H__

#include <linux/types.h>

struct iris_inst;

u32 iris_get_operating_fps(struct iris_inst *inst);
int iris_scale_power(struct iris_inst *inst);
int iris_unvote_power(struct iris_inst *inst);

#endif
