// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/math64.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>

#include "iris_buffer.h"
#include "iris_common.h"
#include "iris_ctrls.h"
#include "iris_hfi_gen1_defines.h"
#include "iris_instance.h"
#include "iris_power.h"
#include "iris_utils.h"
#include "iris_vb2.h"
#include "iris_vdec.h"
#include "iris_vpu_buffer.h"

#define DEFAULT_CODEC_ALIGNMENT 16
#define IRIS1_TIMESTAMP_DISCONTINUITY_NS	NSEC_PER_SEC
#define IRIS1_SEEK_TIMESTAMP_WINDOW_NS	(5ULL * NSEC_PER_SEC)
#define IRIS1_SEEK_HOLD_FRAMES		30
#define IRIS1_MAX_4K_MBPF		NUM_MBS_PER_FRAME(2176, 4096)

int iris_vdec_recycle_pending_output(struct iris_inst *inst)
{
	struct iris_buffer *buf = inst->pending_output;

	if (!buf)
		return 0;

	inst->pending_output = NULL;
	dev_info(inst->core->dev,
		 "Iris1 v155: recycling pending output timestamp %llu ns\n",
		 buf->timestamp);
	buf->data_size = 0;
	buf->flags = 0;
	buf->timestamp = 0;
	buf->attr &= ~(BUF_ATTR_QUEUED | BUF_ATTR_DEQUEUED |
		       BUF_ATTR_BUFFER_DONE);

	return iris_queue_buffer(inst, buf);
}

static void iris_vdec_reset_rate_window(struct iris_inst *inst)
{
	inst->last_buf_ns = 0;
	inst->rate_min_timestamp = 0;
	inst->rate_max_timestamp = 0;
	inst->frame_counter = 0;
}

u32 iris_vdec_get_admission_fps(struct iris_inst *inst)
{
	u64 timestamp_delta_ns;
	u32 frame_rate;

	if (inst->frame_counter > 1 &&
	    inst->rate_max_timestamp > inst->rate_min_timestamp) {
		timestamp_delta_ns = inst->rate_max_timestamp -
				     inst->rate_min_timestamp;
		frame_rate = DIV_ROUND_UP_ULL((u64)(inst->frame_counter - 1) *
					      NSEC_PER_SEC,
					      timestamp_delta_ns);

		return clamp_t(u32, frame_rate, 1, MAXIMUM_FPS);
	}

	/* Preserve 8K30 while treating an unknown 4K stream as 60 fps. */
	if (iris_get_mbpf(inst) > IRIS1_MAX_4K_MBPF)
		return 30;

	return 60;
}

int iris_vdec_complete_pending_output(struct iris_inst *inst)
{
	struct iris_buffer *buf = inst->pending_output;

	if (!buf)
		return 0;

	inst->pending_output = NULL;

	return iris_vb2_buffer_done(inst, buf);
}

int iris_vdec_hold_output(struct iris_inst *inst, struct iris_buffer *buf)
{
	if (inst->pending_output)
		return -EBUSY;

	inst->pending_output = buf;

	return 0;
}

void iris_vdec_clear_pending_output(struct iris_inst *inst)
{
	inst->pending_output = NULL;
}

static int iris_vdec_track_input_timestamp(struct iris_inst *inst, u64 timestamp)
{
	u64 delta;
	int ret;

	if (!inst->core->iris_platform_data->legacy_vpu5)
		return 0;

	if (inst->input_timestamp_valid) {
		delta = timestamp > inst->last_input_timestamp ?
			timestamp - inst->last_input_timestamp :
			inst->last_input_timestamp - timestamp;
		if (delta > IRIS1_TIMESTAMP_DISCONTINUITY_NS) {
			iris_vdec_reset_rate_window(inst);
			inst->seek_timestamp = timestamp;
			inst->seek_timestamp_pending = true;
			inst->seek_hold_frames = IRIS1_SEEK_HOLD_FRAMES;
			dev_info(inst->core->dev,
				 "Iris1 v136: input timestamp discontinuity %llu -> %llu ns; filtering stale output\n",
				 inst->last_input_timestamp, timestamp);
			ret = iris_vdec_recycle_pending_output(inst);
			if (ret)
				return ret;
		}
	}

	inst->last_input_timestamp = timestamp;
	inst->input_timestamp_valid = true;

	return 0;
}

bool iris_vdec_discard_stale_frame(struct iris_inst *inst, u64 timestamp)
{
	u64 delta;

	if (!inst->seek_timestamp_pending)
		return false;

	/*
	 * A frame decoded from pre-seek input carries a timestamp strictly
	 * before the seek anchor even when it lands within the window, because
	 * the anchor is the first post-seek compressed-input timestamp.  Accept
	 * only new-epoch frames at or after the anchor; anything older would be
	 * delivered after the seek and trigger an invalid old/new-epoch
	 * transition in the consumer.
	 */
	if (timestamp >= inst->seek_timestamp) {
		delta = timestamp - inst->seek_timestamp;
		if (delta <= IRIS1_SEEK_TIMESTAMP_WINDOW_NS) {
			dev_info(inst->core->dev,
				 "Iris1 v136: output synchronized at %llu ns after seek anchor %llu ns\n",
				 timestamp, inst->seek_timestamp);
			inst->seek_timestamp_pending = false;
			return false;
		}
	}

	dev_info(inst->core->dev,
		 "Iris1 v136: recycling stale output timestamp %llu ns (seek anchor %llu ns)\n",
		 timestamp, inst->seek_timestamp);

	return true;
}

int iris_vdec_inst_init(struct iris_inst *inst)
{
	struct iris_core *core = inst->core;
	struct v4l2_format *f;
	int ret;

	inst->fmt_src  = kzalloc(sizeof(*inst->fmt_src), GFP_KERNEL);
	inst->fmt_dst  = kzalloc(sizeof(*inst->fmt_dst), GFP_KERNEL);
	if (!inst->fmt_src || !inst->fmt_dst) {
		ret = -ENOMEM;
		goto error_free_formats;
	}

	/* Zero means no sequence requirements have been received yet. */
	inst->fw_min_count = inst->core->iris_platform_data->legacy_vpu5 ? 0 : MIN_BUFFERS;

	f = inst->fmt_src;
	f->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	f->fmt.pix_mp.width = DEFAULT_WIDTH;
	f->fmt.pix_mp.height = DEFAULT_HEIGHT;
	f->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
	inst->codec = f->fmt.pix_mp.pixelformat;
	f->fmt.pix_mp.num_planes = 1;
	f->fmt.pix_mp.plane_fmt[0].bytesperline = 0;
	f->fmt.pix_mp.plane_fmt[0].sizeimage = iris_get_buffer_size(inst, BUF_INPUT);
	f->fmt.pix_mp.field = V4L2_FIELD_NONE;
	inst->buffers[BUF_INPUT].min_count = iris_vpu_buf_count(inst, BUF_INPUT);
	inst->buffers[BUF_INPUT].size = f->fmt.pix_mp.plane_fmt[0].sizeimage;

	f = inst->fmt_dst;
	f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	f->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	f->fmt.pix_mp.width = ALIGN(DEFAULT_WIDTH, 128);
	f->fmt.pix_mp.height = ALIGN(DEFAULT_HEIGHT, 32);
	f->fmt.pix_mp.num_planes = 1;
	f->fmt.pix_mp.plane_fmt[0].bytesperline = ALIGN(DEFAULT_WIDTH, 128);
	f->fmt.pix_mp.plane_fmt[0].sizeimage = iris_get_buffer_size(inst, BUF_OUTPUT);
	f->fmt.pix_mp.field = V4L2_FIELD_NONE;
	f->fmt.pix_mp.colorspace = V4L2_COLORSPACE_DEFAULT;
	f->fmt.pix_mp.xfer_func = V4L2_XFER_FUNC_DEFAULT;
	f->fmt.pix_mp.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	f->fmt.pix_mp.quantization = V4L2_QUANTIZATION_DEFAULT;
	inst->buffers[BUF_OUTPUT].min_count = iris_vpu_buf_count(inst, BUF_OUTPUT);
	inst->buffers[BUF_OUTPUT].size = f->fmt.pix_mp.plane_fmt[0].sizeimage;
	/* Start conservatively, then qbuf throughput updates this once a second. */
	inst->frame_rate = MAXIMUM_FPS;

	memcpy(&inst->fw_caps[0], &core->inst_fw_caps_dec[0],
	       INST_FW_CAP_MAX * sizeof(struct platform_inst_fw_cap));

	ret = iris_ctrls_init(inst);
	if (ret)
		goto error_free_formats;

	return 0;

error_free_formats:
	kfree(inst->fmt_dst);
	kfree(inst->fmt_src);
	inst->fmt_dst = NULL;
	inst->fmt_src = NULL;

	return ret;
}

void iris_vdec_inst_deinit(struct iris_inst *inst)
{
	iris_vdec_clear_pending_output(inst);
	iris_vb2_vp9_release_high_iova_hole(inst);
	kfree(inst->fmt_dst);
	kfree(inst->fmt_src);
}

static const struct iris_fmt iris_vdec_formats[] = {
	[IRIS_FMT_H264] = {
		.pixfmt = V4L2_PIX_FMT_H264,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
	},
	[IRIS_FMT_HEVC] = {
		.pixfmt = V4L2_PIX_FMT_HEVC,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
	},
	[IRIS_FMT_VP9] = {
		.pixfmt = V4L2_PIX_FMT_VP9,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
	},
};

static bool iris_vdec_p010_supported(struct iris_inst *inst)
{
	/*
	 * P010 is wired through the HFI Gen1 raw-format contract below.  HFI
	 * Gen2 still advertises an 8-bit-only BIT_DEPTH capability.
	 */
	return !inst->core->iris_platform_data->core_arch;
}

u32 iris_vdec_raw_stride(u32 width, u32 pixelformat)
{
	u32 alignment = pixelformat == V4L2_PIX_FMT_P010 ? 256 : 128;
	u32 bytes_per_sample = pixelformat == V4L2_PIX_FMT_P010 ? 2 : 1;

	return ALIGN(width * bytes_per_sample, alignment);
}

static const struct iris_fmt *
find_format(struct iris_inst *inst, u32 pixfmt, u32 type)
{
	unsigned int size = ARRAY_SIZE(iris_vdec_formats);
	const struct iris_fmt *fmt = iris_vdec_formats;
	unsigned int i;

	for (i = 0; i < size; i++) {
		if (fmt[i].pixfmt == pixfmt)
			break;
	}

	if (i == size || fmt[i].type != type)
		return NULL;

	return &fmt[i];
}

static const struct iris_fmt *
find_format_by_index(struct iris_inst *inst, u32 index, u32 type)
{
	const struct iris_fmt *fmt = iris_vdec_formats;
	unsigned int size = ARRAY_SIZE(iris_vdec_formats);

	if (index >= size || fmt[index].type != type)
		return NULL;

	return &fmt[index];
}

int iris_vdec_enum_fmt(struct iris_inst *inst, struct v4l2_fmtdesc *f)
{
	const struct iris_fmt *fmt;

	switch (f->type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		fmt = find_format_by_index(inst, f->index, f->type);
		if (!fmt)
			return -EINVAL;

		f->pixelformat = fmt->pixfmt;
		f->flags = V4L2_FMT_FLAG_COMPRESSED | V4L2_FMT_FLAG_DYN_RESOLUTION;
		break;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		if (f->index == 0)
			f->pixelformat = V4L2_PIX_FMT_NV12;
		else if (f->index == 1 && iris_vdec_p010_supported(inst))
			f->pixelformat = V4L2_PIX_FMT_P010;
		else
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

int iris_vdec_try_fmt(struct iris_inst *inst, struct v4l2_format *f)
{
	const struct platform_inst_caps *caps = inst->core->iris_platform_data->inst_caps;
	struct v4l2_pix_format_mplane *pixmp = &f->fmt.pix_mp;
	struct v4l2_m2m_ctx *m2m_ctx = inst->m2m_ctx;
	const struct iris_fmt *fmt;
	struct v4l2_format *f_inst;
	struct vb2_queue *src_q;

	memset(pixmp->reserved, 0, sizeof(pixmp->reserved));
	fmt = find_format(inst, pixmp->pixelformat, f->type);
	switch (f->type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		if (!fmt) {
			f_inst = inst->fmt_src;
			f->fmt.pix_mp.width = f_inst->fmt.pix_mp.width;
			f->fmt.pix_mp.height = f_inst->fmt.pix_mp.height;
			f->fmt.pix_mp.pixelformat = f_inst->fmt.pix_mp.pixelformat;
		}
		/* OUTPUT dimensions are hints until the first source-change event.
		 * Return supported allocation dimensions instead of accepting an
		 * out-of-range hint here and rejecting it later at REQBUFS.
		 */
		pixmp->width = clamp(pixmp->width, caps->min_frame_width,
				     caps->max_frame_width);
		pixmp->height = clamp(pixmp->height, caps->min_frame_height,
				      caps->max_frame_height);
		break;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		if (f->fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 &&
		    (f->fmt.pix_mp.pixelformat != V4L2_PIX_FMT_P010 ||
		     !iris_vdec_p010_supported(inst))) {
			f_inst = inst->fmt_dst;
			f->fmt.pix_mp.pixelformat = f_inst->fmt.pix_mp.pixelformat;
			f->fmt.pix_mp.width = f_inst->fmt.pix_mp.width;
			f->fmt.pix_mp.height = f_inst->fmt.pix_mp.height;
		}

		src_q = v4l2_m2m_get_src_vq(m2m_ctx);
		if (vb2_is_streaming(src_q)) {
			f_inst = inst->fmt_src;
			f->fmt.pix_mp.height = f_inst->fmt.pix_mp.height;
			f->fmt.pix_mp.width = f_inst->fmt.pix_mp.width;
		}
		break;
	default:
		return -EINVAL;
	}

	if (pixmp->field == V4L2_FIELD_ANY)
		pixmp->field = V4L2_FIELD_NONE;

	pixmp->num_planes = 1;

	return 0;
}

int iris_vdec_s_fmt(struct iris_inst *inst, struct v4l2_format *f)
{
	struct v4l2_format *fmt, *output_fmt;
	struct vb2_queue *q;
	u32 codec_align, raw_pixfmt;

	q = v4l2_m2m_get_vq(inst->m2m_ctx, f->type);
	if (!q)
		return -EINVAL;

	if (vb2_is_busy(q))
		return -EBUSY;

	iris_vdec_try_fmt(inst, f);

	switch (f->type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		if (!(find_format(inst, f->fmt.pix_mp.pixelformat, f->type)))
			return -EINVAL;

		fmt = inst->fmt_src;
		fmt->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		fmt->fmt.pix_mp.pixelformat = f->fmt.pix_mp.pixelformat;
		inst->codec = fmt->fmt.pix_mp.pixelformat;
		codec_align = inst->codec == V4L2_PIX_FMT_HEVC ? 32 : 16;
		fmt->fmt.pix_mp.width = ALIGN(f->fmt.pix_mp.width, codec_align);
		fmt->fmt.pix_mp.height = ALIGN(f->fmt.pix_mp.height, codec_align);
		fmt->fmt.pix_mp.num_planes = 1;
		fmt->fmt.pix_mp.plane_fmt[0].bytesperline = 0;
		fmt->fmt.pix_mp.plane_fmt[0].sizeimage = iris_get_buffer_size(inst, BUF_INPUT);
		inst->buffers[BUF_INPUT].min_count = iris_vpu_buf_count(inst, BUF_INPUT);
		inst->buffers[BUF_INPUT].size = fmt->fmt.pix_mp.plane_fmt[0].sizeimage;

		fmt->fmt.pix_mp.colorspace = f->fmt.pix_mp.colorspace;
		fmt->fmt.pix_mp.xfer_func = f->fmt.pix_mp.xfer_func;
		fmt->fmt.pix_mp.ycbcr_enc = f->fmt.pix_mp.ycbcr_enc;
		fmt->fmt.pix_mp.quantization = f->fmt.pix_mp.quantization;

		output_fmt = inst->fmt_dst;
		output_fmt->fmt.pix_mp.colorspace = f->fmt.pix_mp.colorspace;
		output_fmt->fmt.pix_mp.xfer_func = f->fmt.pix_mp.xfer_func;
		output_fmt->fmt.pix_mp.ycbcr_enc = f->fmt.pix_mp.ycbcr_enc;
		output_fmt->fmt.pix_mp.quantization = f->fmt.pix_mp.quantization;
		raw_pixfmt = output_fmt->fmt.pix_mp.pixelformat;

		/* Update capture format based on new ip w/h */
		output_fmt->fmt.pix_mp.width = ALIGN(f->fmt.pix_mp.width, 128);
		output_fmt->fmt.pix_mp.height = ALIGN(f->fmt.pix_mp.height, 32);
		output_fmt->fmt.pix_mp.plane_fmt[0].bytesperline =
			iris_vdec_raw_stride(f->fmt.pix_mp.width, raw_pixfmt);
		output_fmt->fmt.pix_mp.plane_fmt[0].sizeimage =
			iris_get_buffer_size(inst, BUF_OUTPUT);
		inst->buffers[BUF_OUTPUT].size = iris_get_buffer_size(inst, BUF_OUTPUT);

		inst->crop.left = 0;
		inst->crop.top = 0;
		inst->crop.width = f->fmt.pix_mp.width;
		inst->crop.height = f->fmt.pix_mp.height;
		break;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		fmt = inst->fmt_dst;
		fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		if (f->fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 &&
		    (f->fmt.pix_mp.pixelformat != V4L2_PIX_FMT_P010 ||
		     !iris_vdec_p010_supported(inst)))
			return -EINVAL;
		fmt->fmt.pix_mp.pixelformat = f->fmt.pix_mp.pixelformat;
		raw_pixfmt = fmt->fmt.pix_mp.pixelformat;
		fmt->fmt.pix_mp.width = ALIGN(f->fmt.pix_mp.width, 128);
		fmt->fmt.pix_mp.height = ALIGN(f->fmt.pix_mp.height, 32);
		fmt->fmt.pix_mp.num_planes = 1;
		fmt->fmt.pix_mp.plane_fmt[0].bytesperline =
			iris_vdec_raw_stride(f->fmt.pix_mp.width, raw_pixfmt);
		fmt->fmt.pix_mp.plane_fmt[0].sizeimage = iris_get_buffer_size(inst, BUF_OUTPUT);
		inst->buffers[BUF_OUTPUT].min_count = iris_vpu_buf_count(inst, BUF_OUTPUT);
		inst->buffers[BUF_OUTPUT].size = fmt->fmt.pix_mp.plane_fmt[0].sizeimage;

		inst->crop.top = 0;
		inst->crop.left = 0;
		inst->crop.width = f->fmt.pix_mp.width;
		inst->crop.height = f->fmt.pix_mp.height;
		break;
	default:
		return -EINVAL;
	}
	memcpy(f, fmt, sizeof(*fmt));

	return 0;
}

int iris_vdec_validate_format(struct iris_inst *inst, u32 pixelformat)
{
	const struct iris_fmt *fmt = NULL;

	if (pixelformat != V4L2_PIX_FMT_NV12 &&
	    (pixelformat != V4L2_PIX_FMT_P010 ||
	     !iris_vdec_p010_supported(inst))) {
		fmt = find_format(inst, pixelformat, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
		if (!fmt)
			return -EINVAL;
	}

	return 0;
}

int iris_vdec_subscribe_event(struct iris_inst *inst, const struct v4l2_event_subscription *sub)
{
	int ret = 0;

	switch (sub->type) {
	case V4L2_EVENT_EOS:
		ret = v4l2_event_subscribe(&inst->fh, sub, 0, NULL);
		break;
	case V4L2_EVENT_SOURCE_CHANGE:
		ret = v4l2_src_change_event_subscribe(&inst->fh, sub);
		break;
	case V4L2_EVENT_CTRL:
		ret = v4l2_ctrl_subscribe_event(&inst->fh, sub);
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

void iris_vdec_src_change(struct iris_inst *inst)
{
	struct v4l2_m2m_ctx *m2m_ctx = inst->m2m_ctx;
	struct v4l2_event event = {0};
	struct vb2_queue *src_q;

	src_q = v4l2_m2m_get_src_vq(m2m_ctx);
	if (!vb2_is_streaming(src_q))
		return;

	event.type = V4L2_EVENT_SOURCE_CHANGE;
	event.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION;
	v4l2_event_queue_fh(&inst->fh, &event);
}

int iris_vdec_streamon_input(struct iris_inst *inst)
{
	const struct iris_hfi_command_ops *hfi_ops = inst->core->hfi_ops;
	u32 output_order = HFI_OUTPUT_ORDER_DECODE;
	int ret;

	inst->streamoff_pending = false;
	inst->input_timestamp_valid = false;
	inst->seek_timestamp_pending = false;
	inst->corrupt_output_drops = 0;
	inst->pending_output = NULL;
	inst->seek_hold_frames = 0;
	inst->frame_rate = MAXIMUM_FPS;
	inst->frame_rate_down_count = 0;

	ret = iris_set_properties(inst, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	if (ret) {
		dev_err(inst->core->dev,
			"Iris1 v119: input streamon set_properties failed: %d\n", ret);
		return ret;
	}

	if (inst->core->iris_platform_data->legacy_vpu5 &&
	    inst->display_delay_enable && !inst->display_delay) {
		ret = hfi_ops->session_set_property(inst,
				 HFI_PROPERTY_PARAM_VDEC_OUTPUT_ORDER,
				 HFI_HOST_FLAGS_NONE, HFI_PORT_BITSTREAM,
				 HFI_PAYLOAD_U32_ENUM, &output_order,
				 sizeof(output_order));
		if (ret) {
			dev_err(inst->core->dev,
				"Iris1: setting decode-order output failed: %d\n",
				ret);
			return ret;
		}
	}

	ret = iris_alloc_and_queue_persist_bufs(inst, BUF_PERSIST);
	if (ret) {
		dev_err(inst->core->dev,
			"Iris1 v119: input streamon persist buffers failed: %d\n", ret);
		return ret;
	}

	iris_get_internal_buffers(inst, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);

	ret = iris_destroy_dequeued_internal_buffers(inst, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	if (ret) {
		dev_err(inst->core->dev,
			"Iris1 v119: input streamon destroy buffers failed: %d\n", ret);
		return ret;
	}

	ret = iris_create_internal_buffers(inst, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	if (ret) {
		dev_err(inst->core->dev,
			"Iris1 v119: input streamon create buffers failed: %d\n", ret);
		return ret;
	}

	ret = iris_queue_internal_buffers(inst, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	if (ret) {
		dev_err(inst->core->dev,
			"Iris1 v119: input streamon queue buffers failed: %d\n", ret);
		return ret;
	}

	iris_vdec_reset_rate_window(inst);

	return iris_process_streamon_input(inst);
}

int iris_vdec_streamon_output(struct iris_inst *inst)
{
	const struct iris_hfi_command_ops *hfi_ops = inst->core->hfi_ops;
	int ret;

	inst->streamoff_pending = false;

	ret = hfi_ops->session_set_config_params(inst, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (ret) {
		dev_err(inst->core->dev,
			"Iris1 v119: capture streamon config failed: %d\n", ret);
		return ret;
	}

	iris_get_internal_buffers(inst, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

	ret = iris_destroy_dequeued_internal_buffers(inst, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (ret) {
		dev_err(inst->core->dev,
			"Iris1 v119: capture streamon destroy buffers failed: %d\n", ret);
		return ret;
	}

	ret = iris_create_internal_buffers(inst, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (ret) {
		dev_err(inst->core->dev,
			"Iris1 v119: capture streamon create buffers failed: %d\n", ret);
		return ret;
	}

	ret = iris_process_streamon_output(inst);
	if (ret) {
		dev_err(inst->core->dev,
			"Iris1 v119: capture streamon process failed: %d\n", ret);
		goto error;
	}

	ret = iris_queue_internal_buffers(inst, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (ret) {
		dev_err(inst->core->dev,
			"Iris1 v119: capture streamon queue buffers failed: %d\n", ret);
		goto error;
	}

	return ret;

error:
	inst->start_streaming_rollback_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	iris_session_streamoff(inst, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	inst->start_streaming_rollback_type = 0;

	return ret;
}

int iris_vdec_qbuf(struct iris_inst *inst, struct vb2_v4l2_buffer *vbuf)
{
	struct iris_buffer *buf = to_iris_buffer(vbuf);
	struct vb2_buffer *vb2 = &vbuf->vb2_buf;
	u64 cur_buf_ns, delta_ns, timestamp_delta_ns;
	u32 measured_fps, qbuf_fps, timestamp_fps, target_fps;
	struct vb2_queue *q;
	int ret;

	ret = iris_vb2_buffer_to_driver(vb2, buf);
	if (ret)
		return ret;

	if (buf->type == BUF_INPUT)
		iris_set_ts_metadata(inst, vbuf);

	q = v4l2_m2m_get_vq(inst->m2m_ctx, vb2->type);
	if (!vb2_is_streaming(q)) {
		buf->attr |= BUF_ATTR_DEFERRED;
		return 0;
	}

	if (buf->type == BUF_INPUT) {
		ret = iris_vdec_track_input_timestamp(inst, buf->timestamp);
		if (ret)
			return ret;
	}

	/*
	 * QBUF arrival can be bursty and, once the decoder or the consumer is
	 * under-provisioned, can be limited by CAPTURE backpressure.  Using only
	 * that arrival rate creates a feedback loop: a 60 fps stream that falls
	 * behind is measured below 60 fps and receives an even lower power vote.
	 *
	 * Measure both wall-clock QBUF turnover and the coded timestamp span.
	 * Min/max timestamps tolerate normal B-frame reordering, and taking the
	 * larger estimate keeps a temporarily backpressured stream at its coded
	 * rate.  Start every stream at the maximum vote and retain the existing
	 * hysteresis for sustained reductions.
	 */
	if (buf->type == BUF_INPUT) {
		cur_buf_ns = ktime_get_ns();

		if (!inst->frame_counter) {
			inst->last_buf_ns = cur_buf_ns;
			inst->rate_min_timestamp = buf->timestamp;
			inst->rate_max_timestamp = buf->timestamp;
		} else {
			inst->rate_min_timestamp =
				min(inst->rate_min_timestamp, buf->timestamp);
			inst->rate_max_timestamp =
				max(inst->rate_max_timestamp, buf->timestamp);
		}

		inst->frame_counter++;
		delta_ns = cur_buf_ns - inst->last_buf_ns;

		if (delta_ns >= NSEC_PER_SEC) {
			/* N frames contain N - 1 frame intervals. */
			qbuf_fps = div64_u64((u64)(inst->frame_counter - 1) *
						 NSEC_PER_SEC + delta_ns - 1,
						 delta_ns);
			timestamp_delta_ns = inst->rate_max_timestamp -
						     inst->rate_min_timestamp;
			timestamp_fps = 0;
			if (inst->core->iris_platform_data->legacy_vpu5 &&
			    timestamp_delta_ns && inst->frame_counter > 1)
				timestamp_fps = div64_u64((u64)(inst->frame_counter - 1) *
							 NSEC_PER_SEC +
							 timestamp_delta_ns - 1,
							 timestamp_delta_ns);

			measured_fps = max(qbuf_fps, timestamp_fps);
			measured_fps = clamp_t(u32, measured_fps, 1, MAXIMUM_FPS);
			target_fps = clamp_t(u32,
					     DIV_ROUND_UP(measured_fps * 11, 10),
					     DEFAULT_FPS, MAXIMUM_FPS);

			/* Raise immediately; require three lower windows to reduce. */
			if (inst->frame_rate == MAXIMUM_FPS ||
			    target_fps >= inst->frame_rate) {
				inst->frame_rate = target_fps;
				inst->frame_rate_down_count = 0;
			} else if (++inst->frame_rate_down_count >= 3) {
				inst->frame_rate = target_fps;
				inst->frame_rate_down_count = 0;
			}

			dev_info(inst->core->dev,
				 "Iris1 v140: measured input %u fps (qbuf %u, timestamps %u), voting %u fps (down windows %u)\n",
				 measured_fps, qbuf_fps, timestamp_fps, inst->frame_rate,
				 inst->frame_rate_down_count);
			iris_vdec_reset_rate_window(inst);
		}
	}

	iris_scale_power(inst);

	return iris_queue_buffer(inst, buf);
}

int iris_vdec_start_cmd(struct iris_inst *inst)
{
	const struct iris_hfi_command_ops *hfi_ops = inst->core->hfi_ops;
	enum iris_inst_sub_state clear_sub_state = 0;
	struct vb2_queue *dst_vq;
	int ret;

	dst_vq = v4l2_m2m_get_dst_vq(inst->m2m_ctx);

	if (inst->capture_format_changed) {
		dev_err(inst->core->dev, "CAPTURE format changed; reallocate before START\n");
		return -EINVAL;
	}

	/* START on an already running decoder is a no-op. In particular the
	 * initial source-change event need not have drained an early CAPTURE.
	 */
	if (inst->state == IRIS_INST_STREAMING &&
	    !(inst->sub_state & (IRIS_INST_SUB_DRC | IRIS_INST_SUB_DRAIN)))
		return 0;

	/* A same-size source change can resume via START without STREAMOFF.
	 * Wait until the legacy output flush has returned its DPBs before
	 * requeueing them; otherwise a late flush completion strands the pool.
	 */
	if (inst->core->iris_platform_data->legacy_vpu5 &&
	    inst->flush_responses_pending) {
		ret = iris_wait_for_session_response(inst, true);
		if (ret)
			return ret;
	}

	if (inst->sub_state & IRIS_INST_SUB_DRC &&
	    inst->sub_state & IRIS_INST_SUB_DRC_LAST) {
		vb2_clear_last_buffer_dequeued(dst_vq);
		clear_sub_state = IRIS_INST_SUB_DRC | IRIS_INST_SUB_DRC_LAST;

		if (inst->sub_state & IRIS_INST_SUB_INPUT_PAUSE) {
			ret = hfi_ops->session_resume_drc(inst,
							  V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
			if (ret)
				return ret;
			clear_sub_state |= IRIS_INST_SUB_INPUT_PAUSE;
		}
		if (inst->sub_state & IRIS_INST_SUB_OUTPUT_PAUSE) {
			ret = hfi_ops->session_resume_drc(inst,
							  V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
			if (ret)
				return ret;
			clear_sub_state |= IRIS_INST_SUB_OUTPUT_PAUSE;
		}
	} else if (inst->sub_state & IRIS_INST_SUB_DRAIN &&
		   inst->sub_state & IRIS_INST_SUB_DRAIN_LAST) {
		vb2_clear_last_buffer_dequeued(dst_vq);
		clear_sub_state = IRIS_INST_SUB_DRAIN | IRIS_INST_SUB_DRAIN_LAST;
		if (inst->sub_state & IRIS_INST_SUB_INPUT_PAUSE) {
			if (hfi_ops->session_resume_drain) {
				ret =
				hfi_ops->session_resume_drain(inst,
							      V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
				if (ret)
					return ret;
			}

			clear_sub_state |= IRIS_INST_SUB_INPUT_PAUSE;
		}
		if (inst->sub_state & IRIS_INST_SUB_OUTPUT_PAUSE) {
			if (hfi_ops->session_resume_drain) {
				ret =
				hfi_ops->session_resume_drain(inst,
							      V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
				if (ret)
					return ret;
			}

			clear_sub_state |= IRIS_INST_SUB_OUTPUT_PAUSE;
		}
	} else {
		dev_err(inst->core->dev, "start called before receiving last_flag\n");
		iris_inst_change_state(inst, IRIS_INST_ERROR);
		return -EBUSY;
	}

	ret = iris_inst_change_sub_state(inst, clear_sub_state, 0);
	if (ret)
		return ret;
	inst->last_buffer_dequeued = false;

	if (inst->core->iris_platform_data->legacy_vpu5 &&
	    inst->state == IRIS_INST_STREAMING) {
		ret = iris_queue_internal_buffers(inst, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
		if (!ret)
			ret = iris_queue_deferred_buffers(inst, BUF_OUTPUT);
		if (!ret)
			ret = iris_queue_deferred_buffers(inst, BUF_INPUT);
	}

	return ret;
}

int iris_vdec_stop_cmd(struct iris_inst *inst)
{
	const struct iris_hfi_command_ops *hfi_ops = inst->core->hfi_ops;
	int ret;

	ret = hfi_ops->session_drain(inst, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	if (ret)
		return ret;

	return iris_inst_change_sub_state(inst, 0, IRIS_INST_SUB_DRAIN);
}
