/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __IRIS_VIDC_H__
#define __IRIS_VIDC_H__

#include <linux/videodev2.h>

struct iris_core;

struct iris_surface_fence_cmd {
	__u32 op;
	__s32 dmabuf_fd;
	__u64 token;
};

#define IRIS_SURFACE_FENCE_ATTACH	1
#define IRIS_SURFACE_FENCE_SIGNAL	2
#define VIDIOC_IRIS_SURFACE_FENCE \
	_IOW('V', BASE_VIDIOC_PRIVATE, struct iris_surface_fence_cmd)

void iris_init_ops(struct iris_core *core);
int iris_open(struct file *filp);
int iris_close(struct file *filp);

#endif
