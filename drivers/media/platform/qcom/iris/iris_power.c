// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include <media/v4l2-mem2mem.h>

#include "iris_buffer.h"
#include "iris_instance.h"
#include "iris_power.h"
#include "iris_resources.h"
#include "iris_vpu_common.h"

u32 iris_get_operating_fps(struct iris_inst *inst)
{
	return inst->frame_rate ?: DEFAULT_FPS;
}

static u32 iris_calc_bw(struct iris_inst *inst, struct icc_vote_data *data)
{
	const struct bw_info *bw_tbl = NULL;
	struct iris_core *core = inst->core;
	u32 num_rows, i, mbs, mbps;
	u32 icc_bw = 0;

	mbs = DIV_ROUND_UP(data->height, 16) * DIV_ROUND_UP(data->width, 16);
	mbps = mbs * data->fps;
	if (mbps == 0)
		goto exit;

	bw_tbl = core->iris_platform_data->bw_tbl_dec;
	num_rows = core->iris_platform_data->bw_tbl_dec_size;

	for (i = 0; i < num_rows; i++) {
		if (i != 0 && mbps > bw_tbl[i].mbs_per_sec)
			break;

		icc_bw = bw_tbl[i].bw_ddr;
	}

exit:
	return icc_bw;
}

static int iris_set_interconnects(struct iris_inst *inst)
{
	struct iris_core *core = inst->core;
	struct iris_inst *instance;
	u64 total_bw_ddr = 0;
	int ret;

	/*
	 * The legacy VPU5 RPMh path can stop completing active interconnect
	 * votes when several 4K sessions are running.  iris_vpu_power_on()
	 * already establishes the maximum video-mem vote before firmware boot,
	 * so retain that vote until iris_vpu_power_off() removes it.  Runtime
	 * scaling remains enabled for newer hardware.
	 */
	if (core->iris_platform_data->legacy_vpu5)
		return 0;

	mutex_lock(&core->lock);
	list_for_each_entry(instance, &core->instances, list) {
		if (!instance->hfi_session_opened ||
		    !instance->max_input_data_size)
			continue;

		total_bw_ddr += instance->power.icc_bw;
	}

	ret = iris_set_icc_bw(core, total_bw_ddr);

	mutex_unlock(&core->lock);

	return ret;
}

static int iris_vote_interconnects(struct iris_inst *inst)
{
	struct icc_vote_data *vote_data = &inst->icc_data;
	struct v4l2_format *inp_f = inst->fmt_src;

	vote_data->width = inp_f->fmt.pix_mp.width;
	vote_data->height = inp_f->fmt.pix_mp.height;
	vote_data->fps = iris_get_operating_fps(inst);

	inst->power.icc_bw = iris_calc_bw(inst, vote_data);

	return iris_set_interconnects(inst);
}

static int iris_set_clocks(struct iris_inst *inst)
{
	struct iris_core *core = inst->core;
	struct iris_inst *instance;
	u64 freq = 0;
	int ret;

	/*
	 * iris_vpu_power_on() already selects the maximum OPP for legacy VPU5.
	 * Changing that OPP while VideoCC/MMCX is active can wedge the RPMh TCS,
	 * so keep it until iris_vpu_power_off() clears the vote.  Newer hardware
	 * continues to use per-session runtime clock scaling.
	 */
	if (core->iris_platform_data->legacy_vpu5)
		return 0;

	mutex_lock(&core->lock);
	list_for_each_entry(instance, &core->instances, list) {
		if (!instance->hfi_session_opened ||
		    !instance->max_input_data_size)
			continue;

		freq += instance->power.min_freq;
	}

	if (freq == core->power.clk_freq) {
		ret = 0;
		goto unlock;
	}

	ret = dev_pm_opp_set_rate(core->dev, freq);
	if (!ret)
		core->power.clk_freq = freq;

unlock:
	mutex_unlock(&core->lock);

	return ret;
}

static int iris_scale_clocks(struct iris_inst *inst)
{
	const struct vpu_ops *vpu_ops = inst->core->iris_platform_data->vpu_ops;
	struct v4l2_m2m_ctx *m2m_ctx = inst->m2m_ctx;
	struct v4l2_m2m_buffer *buffer, *n;
	struct iris_buffer *buf;
	size_t data_size = 0;

	v4l2_m2m_for_each_src_buf_safe(m2m_ctx, buffer, n) {
		buf = to_iris_buffer(&buffer->vb);
		data_size = max(data_size, buf->data_size);
	}

	inst->max_input_data_size = data_size;
	if (!inst->max_input_data_size)
		return 0;

	inst->power.min_freq = vpu_ops->calc_freq(inst, inst->max_input_data_size);

	return iris_set_clocks(inst);
}

int iris_scale_power(struct iris_inst *inst)
{
	struct iris_core *core = inst->core;
	int ret;

	if (pm_runtime_suspended(core->dev)) {
		ret = pm_runtime_resume_and_get(core->dev);
		if (ret < 0)
			return ret;

		pm_runtime_put_autosuspend(core->dev);
	}

	/*
	 * Legacy VPU5 holds the maximum OPP and interconnect vote for the whole
	 * power session, so per-qbuf clock/interconnect scaling is a no-op there
	 * (iris_set_clocks() and iris_set_interconnects() both return early).
	 * Skipping the per-qbuf input-buffer scan avoids an O(N) walk on every
	 * QBUF in the decode path.
	 */
	if (core->iris_platform_data->legacy_vpu5)
		return 0;

	ret = iris_scale_clocks(inst);
	if (ret)
		return ret;

	return iris_vote_interconnects(inst);
}

int iris_unvote_power(struct iris_inst *inst)
{
	int ret, ret2;

	inst->max_input_data_size = 0;
	inst->power.min_freq = 0;
	inst->power.icc_bw = 0;

	ret = iris_set_clocks(inst);
	ret2 = iris_set_interconnects(inst);

	return ret ?: ret2;
}
