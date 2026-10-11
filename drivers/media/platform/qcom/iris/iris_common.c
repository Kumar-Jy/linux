// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <media/v4l2-mem2mem.h>

#include "iris_common.h"
#include "iris_ctrls.h"
#include "iris_instance.h"
#include "iris_power.h"
#include "iris_utils.h"

#define IRIS1_4K_MBPF			NUM_MBS_PER_FRAME(2160, 3840)
#define IRIS1_MAX_4K_SESSIONS		3

int iris_vb2_buffer_to_driver(struct vb2_buffer *vb2, struct iris_buffer *buf)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb2);

	buf->type = iris_v4l2_type_to_driver(vb2->type);
	buf->index = vb2->index;
	buf->fd = vb2->planes[0].m.fd;
	buf->buffer_size = vb2->planes[0].length;
	buf->data_offset = vb2->planes[0].data_offset;
	buf->data_size = vb2->planes[0].bytesused - vb2->planes[0].data_offset;
	buf->flags = vbuf->flags;
	buf->timestamp = vb2->timestamp;
	buf->attr = 0;

	return 0;
}

void iris_set_ts_metadata(struct iris_inst *inst, struct vb2_v4l2_buffer *vbuf)
{
	u32 mask = V4L2_BUF_FLAG_TIMECODE | V4L2_BUF_FLAG_TSTAMP_SRC_MASK;
	struct vb2_buffer *vb = &vbuf->vb2_buf;
	u64 ts_us = vb->timestamp;

	if (inst->metadata_idx >= ARRAY_SIZE(inst->tss))
		inst->metadata_idx = 0;

	do_div(ts_us, NSEC_PER_USEC);

	inst->tss[inst->metadata_idx].flags = vbuf->flags & mask;
	inst->tss[inst->metadata_idx].tc = vbuf->timecode;
	inst->tss[inst->metadata_idx].ts_us = ts_us;
	inst->tss[inst->metadata_idx].ts_ns = vb->timestamp;

	inst->metadata_idx++;
}

int iris_process_streamon_input(struct iris_inst *inst)
{
	const struct iris_hfi_command_ops *hfi_ops = inst->core->hfi_ops;
	enum iris_inst_sub_state set_sub_state = 0;
	int ret;

	inst->streamoff_pending = false;

	iris_scale_power(inst);

	ret = hfi_ops->session_start(inst, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	if (ret)
		return ret;

	if (inst->sub_state & IRIS_INST_SUB_INPUT_PAUSE) {
		ret = iris_inst_change_sub_state(inst, IRIS_INST_SUB_INPUT_PAUSE, 0);
		if (ret)
			return ret;
	}

	if (inst->domain == DECODER &&
	    (inst->sub_state & IRIS_INST_SUB_DRC ||
	     inst->sub_state & IRIS_INST_SUB_DRAIN ||
	     inst->sub_state & IRIS_INST_SUB_FIRST_IPSC)) {
		if (!(inst->sub_state & IRIS_INST_SUB_INPUT_PAUSE)) {
			if (hfi_ops->session_pause) {
				ret = hfi_ops->session_pause(inst,
							     V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
				if (ret)
					return ret;
			}
			set_sub_state = IRIS_INST_SUB_INPUT_PAUSE;
		}
	}

	ret = iris_inst_state_change_streamon(inst, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	if (ret)
		return ret;

	inst->last_buffer_dequeued = false;

	return iris_inst_change_sub_state(inst, 0, set_sub_state);
}

int iris_process_streamon_output(struct iris_inst *inst)
{
	const struct iris_hfi_command_ops *hfi_ops = inst->core->hfi_ops;
	bool drain_active = false, drc_active = false;
	enum iris_inst_sub_state clear_sub_state = 0;
	int ret = 0;

	iris_scale_power(inst);

	drain_active = inst->sub_state & IRIS_INST_SUB_DRAIN &&
		inst->sub_state & IRIS_INST_SUB_DRAIN_LAST;

	drc_active = inst->sub_state & IRIS_INST_SUB_DRC &&
		inst->sub_state & IRIS_INST_SUB_DRC_LAST;

	if (drc_active)
		clear_sub_state = IRIS_INST_SUB_DRC | IRIS_INST_SUB_DRC_LAST;
	else if (drain_active)
		clear_sub_state = IRIS_INST_SUB_DRAIN | IRIS_INST_SUB_DRAIN_LAST;

	if (inst->domain == DECODER && inst->sub_state & IRIS_INST_SUB_INPUT_PAUSE) {
		ret = iris_alloc_and_queue_input_int_bufs(inst);
		if (ret)
			return ret;
		ret = iris_set_stage(inst, STAGE);
		if (ret)
			return ret;
		ret = iris_set_pipe(inst, PIPE);
		if (ret)
			return ret;
	}

	if (inst->state == IRIS_INST_INPUT_STREAMING &&
	    inst->sub_state & IRIS_INST_SUB_INPUT_PAUSE) {
		if (!drain_active)
			ret = hfi_ops->session_resume_drc(inst,
							  V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
		else if (hfi_ops->session_resume_drain)
			ret = hfi_ops->session_resume_drain(inst,
							    V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
		if (ret)
			return ret;
		clear_sub_state |= IRIS_INST_SUB_INPUT_PAUSE;
	}

	if (inst->sub_state & IRIS_INST_SUB_FIRST_IPSC)
		clear_sub_state |= IRIS_INST_SUB_FIRST_IPSC;

	ret = hfi_ops->session_start(inst, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (ret)
		return ret;

	if (inst->sub_state & IRIS_INST_SUB_OUTPUT_PAUSE)
		clear_sub_state |= IRIS_INST_SUB_OUTPUT_PAUSE;

	ret = iris_inst_state_change_streamon(inst, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (ret)
		return ret;

	inst->last_buffer_dequeued = false;

	return iris_inst_change_sub_state(inst, clear_sub_state, 0);
}

static void iris_flush_deferred_buffers(struct iris_inst *inst,
					enum iris_buffer_type type)
{
	struct v4l2_m2m_ctx *m2m_ctx = inst->m2m_ctx;
	struct v4l2_m2m_buffer *buffer, *n;
	struct iris_buffer *buf;

	if (type == BUF_INPUT) {
		v4l2_m2m_for_each_src_buf_safe(m2m_ctx, buffer, n) {
			buf = to_iris_buffer(&buffer->vb);
			if (buf->attr & BUF_ATTR_DEFERRED) {
				if (!(buf->attr & BUF_ATTR_BUFFER_DONE)) {
					buf->attr |= BUF_ATTR_BUFFER_DONE;
					buf->data_size = 0;
					iris_vb2_buffer_done(inst, buf);
				}
			}
		}
	} else {
		v4l2_m2m_for_each_dst_buf_safe(m2m_ctx, buffer, n) {
			buf = to_iris_buffer(&buffer->vb);
			if (buf->attr & BUF_ATTR_DEFERRED) {
				if (!(buf->attr & BUF_ATTR_BUFFER_DONE)) {
					buf->attr |= BUF_ATTR_BUFFER_DONE;
					buf->data_size = 0;
					iris_vb2_buffer_done(inst, buf);
				}
			}
		}
	}
}

static void iris_kill_session(struct iris_inst *inst)
{
	const struct iris_hfi_command_ops *hfi_ops = inst->core->hfi_ops;

	if (!inst->session_id)
		return;

	hfi_ops->session_close(inst);
	iris_inst_change_state(inst, IRIS_INST_ERROR);
}

int iris_hfi_session_open(struct iris_inst *inst)
{
	const struct iris_hfi_command_ops *hfi_ops = inst->core->hfi_ops;
	struct iris_core *core = inst->core;
	struct iris_inst *instance;
	u64 active_mbpf = 0;
	u32 active_sessions = 0;
	int ret;

	if (inst->hfi_session_opened)
		return 0;

	mutex_lock(&core->lock);
	list_for_each_entry(instance, &core->instances, list) {
		if (!instance->hfi_session_opened)
			continue;

		active_sessions++;
		active_mbpf += iris_get_mbpf(instance);
	}

	if (active_sessions >= core->iris_platform_data->max_session_count) {
		mutex_unlock(&core->lock);
		return -EBUSY;
	}

	active_mbpf += iris_get_mbpf(inst);
	if (active_mbpf > core->iris_platform_data->max_core_mbpf) {
		dev_warn(core->dev,
			 "Iris1 v154: rejecting session aggregate mbpf %llu above %u\n",
			 (unsigned long long)active_mbpf,
			 core->iris_platform_data->max_core_mbpf);
		mutex_unlock(&core->lock);
		return -ENOMEM;
	}

	/* Reserve session count and frame capacity before issuing SESSION_INIT. */
	inst->hfi_session_opened = true;
	mutex_unlock(&core->lock);

	ret = hfi_ops->session_open(inst);
	if (ret) {
		/* A timeout leaves firmware ownership uncertain until recovery. */
		if (inst->state == IRIS_INST_ERROR)
			return ret;
		goto release_slot;
	}

	if (inst->state == IRIS_INST_ERROR)
		return -EIO;

	ret = iris_inst_change_state(inst, IRIS_INST_INIT);
	if (ret)
		return ret;

	return 0;

release_slot:
	mutex_lock(&core->lock);
	inst->hfi_session_opened = false;
	inst->hfi_core_id = 0;
	inst->core_load_reserved = false;
	inst->core_load_rejected = false;
	inst->admission_frame_rate = 0;
	mutex_unlock(&core->lock);

	return ret;
}

int iris_hfi_session_close(struct iris_inst *inst)
{
	const struct iris_hfi_command_ops *hfi_ops = inst->core->hfi_ops;
	struct iris_core *core = inst->core;
	bool was_error;
	int ret, power_ret;

	if (!inst->hfi_session_opened) {
		iris_release_core_load(inst);
		return 0;
	}

	was_error = inst->state == IRIS_INST_ERROR;
	reinit_completion(&inst->completion);

	ret = hfi_ops->session_close(inst);
	if (ret)
		goto cleanup;

	ret = iris_wait_for_session_response(inst, false);
	if (ret)
		goto cleanup;

	if (!was_error && inst->state == IRIS_INST_ERROR) {
		ret = -EIO;
		goto cleanup;
	}

	if (inst->state != IRIS_INST_ERROR) {
		ret = iris_inst_change_state(inst, IRIS_INST_DEINIT);
		if (ret)
			goto cleanup;
	}

	ret = 0;

cleanup:
	/*
	 * A firmware or response failure (for example an already-errored
	 * session whose SESSION_END is not answered) must not leave the
	 * session registered.  Clear the ownership flags unconditionally so a
	 * stale session can no longer hold a firmware slot, a core-load vote,
	 * or a power vote.
	 */
	mutex_lock(&core->lock);
	inst->hfi_session_opened = false;
	inst->hfi_core_id = 0;
	inst->core_load_reserved = false;
	inst->core_load_rejected = false;
	inst->admission_frame_rate = 0;
	mutex_unlock(&core->lock);

	inst->sub_state = 0;
	inst->streamoff_pending = false;
	inst->flush_responses_pending = 0;
	inst->last_buffer_dequeued = false;

	power_ret = iris_unvote_power(inst);
	if (power_ret)
		dev_warn(core->dev,
			 "failed to remove stopped-session power vote: %d\n",
			 power_ret);

	return ret;
}

int iris_reserve_core_load(struct iris_inst *inst, u32 frame_rate)
{
	struct iris_core *core = inst->core;
	struct iris_inst *instance;
	u64 total_mbps = 0;
	u32 large_sessions = 0;
	u32 mbpf;

	frame_rate = max(frame_rate, 1U);
	mbpf = iris_get_mbpf(inst);

	mutex_lock(&core->lock);
	if (inst->core_load_reserved) {
		mutex_unlock(&core->lock);
		return 0;
	}

	list_for_each_entry(instance, &core->instances, list) {
		if (!instance->core_load_reserved)
			continue;

		total_mbps += (u64)iris_get_mbpf(instance) *
			      instance->admission_frame_rate;
		if (iris_get_mbpf(instance) >= IRIS1_4K_MBPF)
			large_sessions++;
	}

	total_mbps += (u64)mbpf * frame_rate;
	if (total_mbps > core->iris_platform_data->max_core_mbps) {
		dev_warn(core->dev,
			 "Iris1 v154: rejecting aggregate load %llu mb/s above %u (session %u mb/f at %u fps)\n",
			 (unsigned long long)total_mbps,
			 core->iris_platform_data->max_core_mbps,
			 mbpf, frame_rate);
		mutex_unlock(&core->lock);
		return -ENOMEM;
	}

	/*
	 * VIDEO.IR.1.2 exhausts its firmware VBUF object pool at the fourth
	 * concurrent 4K CAPTURE session; reject that allocation.
	 */
	if (core->iris_platform_data->legacy_vpu5 &&
	    mbpf >= IRIS1_4K_MBPF &&
	    large_sessions >= IRIS1_MAX_4K_SESSIONS) {
		dev_warn(core->dev,
			 "Iris1 v154: rejecting fourth 4K-class firmware capture session\n");
		mutex_unlock(&core->lock);
		return -ENOMEM;
	}

	inst->admission_frame_rate = frame_rate;
	inst->core_load_reserved = true;
	mutex_unlock(&core->lock);

	dev_info(core->dev,
		 "Iris1 v154: reserved %u mb/f at %u fps; aggregate %llu/%u mb/s\n",
		 mbpf, frame_rate, (unsigned long long)total_mbps,
		 core->iris_platform_data->max_core_mbps);

	return 0;
}

void iris_release_core_load(struct iris_inst *inst)
{
	struct iris_core *core = inst->core;

	mutex_lock(&core->lock);
	inst->core_load_reserved = false;
	inst->admission_frame_rate = 0;
	mutex_unlock(&core->lock);
}

int iris_session_streamoff(struct iris_inst *inst, u32 plane)
{
	const struct iris_hfi_command_ops *hfi_ops = inst->core->hfi_ops;
	enum iris_buffer_type buffer_type;
	int ret;

	switch (plane) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		buffer_type = BUF_INPUT;
		break;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		buffer_type = BUF_OUTPUT;
		break;
	default:
		return -EINVAL;
	}

	/* Set this before FLUSH/STOP while inst->state can still be STREAMING. */
	inst->streamoff_pending = true;

	ret = hfi_ops->session_stop(inst, plane);
	if (ret)
		goto error;

	ret = iris_inst_state_change_streamoff(inst, plane);
	if (ret)
		goto error;

	iris_flush_deferred_buffers(inst, buffer_type);

	if (inst->state == IRIS_INST_INIT) {
		ret = iris_destroy_all_internal_buffers(inst,
						V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
		if (ret)
			return ret;

		ret = iris_destroy_all_internal_buffers(inst,
						V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
		if (ret)
			return ret;

		ret = iris_hfi_session_close(inst);
		if (ret)
			return ret;

		dev_info(inst->core->dev,
			 "Iris1 v135: released stopped-session DMA and HFI resources\n");
	}

	return 0;

error:
	iris_kill_session(inst);
	iris_flush_deferred_buffers(inst, buffer_type);

	return ret;
}
