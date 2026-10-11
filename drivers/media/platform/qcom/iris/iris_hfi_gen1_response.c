// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/bitfield.h>
#include <linux/dma-mapping.h>
#include <linux/string.h>
#include <media/v4l2-mem2mem.h>

#include "iris_hfi_gen1.h"
#include "iris_hfi_gen1_defines.h"
#include "iris_instance.h"
#include "iris_utils.h"
#include "iris_vb2.h"
#include "iris_vdec.h"
#include "iris_vpu_buffer.h"

#define IRIS1_MAX_CORRUPT_OUTPUT_DROPS 8

static u32
iris_hfi_gen1_bufreq_count_min(struct iris_inst *inst,
			       const struct hfi_buffer_requirements *bufreq)
{
	/*
	 * HFI 4xx swaps the display hold count and minimum-count fields.
	 * SM8150's VIDEO.IR.1.2 firmware uses that legacy layout, while
	 * SM8250 uses the layout represented by the Iris Gen1 structure.
	 */
	if (inst->core->iris_platform_data->legacy_vpu5)
		return bufreq->hold_count;

	return bufreq->count_min;
}

static void iris_hfi_gen1_read_changed_params(struct iris_inst *inst,
					      struct hfi_msg_event_notify_pkt *pkt)
{
	struct v4l2_pix_format_mplane *pixmp_ip = &inst->fmt_src->fmt.pix_mp;
	struct v4l2_pix_format_mplane *pixmp_op = &inst->fmt_dst->fmt.pix_mp;
	u32 num_properties_changed = pkt->event_data2;
	u8 *data_ptr = (u8 *)&pkt->ext_event_data[0];
	u32 primaries, matrix_coeff, transfer_char;
	struct hfi_dpb_counts *iris_vpu_dpb_count;
	struct hfi_profile_level *profile_level;
	struct hfi_buffer_requirements *bufreq;
	struct hfi_extradata_input_crop *crop;
	struct hfi_colour_space *colour_info;
	struct iris_core *core = inst->core;
	u32 colour_description_present_flag;
	u32 video_signal_type_present_flag;
	struct hfi_event_data event = {0};
	struct hfi_bit_depth *pixel_depth;
	struct hfi_pic_struct *pic_struct;
	struct hfi_framesize *frame_sz;
	struct vb2_queue *dst_q;
	struct v4l2_ctrl *ctrl;
	u32 full_range, ptype;

	do {
		ptype = *((u32 *)data_ptr);
		switch (ptype) {
		case HFI_PROPERTY_PARAM_FRAME_SIZE:
			data_ptr += sizeof(u32);
			frame_sz = (struct hfi_framesize *)data_ptr;
			event.width = frame_sz->width;
			event.height = frame_sz->height;
			data_ptr += sizeof(*frame_sz);
			break;
		case HFI_PROPERTY_PARAM_PROFILE_LEVEL_CURRENT:
			data_ptr += sizeof(u32);
			profile_level = (struct hfi_profile_level *)data_ptr;
			event.profile = profile_level->profile;
			event.level = profile_level->level;
			data_ptr += sizeof(*profile_level);
			break;
		case HFI_PROPERTY_PARAM_VDEC_PIXEL_BITDEPTH:
			data_ptr += sizeof(u32);
			pixel_depth = (struct hfi_bit_depth *)data_ptr;
			event.bit_depth = pixel_depth->bit_depth;
			data_ptr += sizeof(*pixel_depth);
			break;
		case HFI_PROPERTY_PARAM_VDEC_PIC_STRUCT:
			data_ptr += sizeof(u32);
			pic_struct = (struct hfi_pic_struct *)data_ptr;
			event.pic_struct = pic_struct->progressive_only;
			data_ptr += sizeof(*pic_struct);
			break;
		case HFI_PROPERTY_PARAM_VDEC_COLOUR_SPACE:
			data_ptr += sizeof(u32);
			colour_info = (struct hfi_colour_space *)data_ptr;
			event.colour_space = colour_info->colour_space;
			data_ptr += sizeof(*colour_info);
			break;
		case HFI_PROPERTY_CONFIG_VDEC_ENTROPY:
			data_ptr += sizeof(u32);
			event.entropy_mode = *(u32 *)data_ptr;
			data_ptr += sizeof(u32);
			break;
		case HFI_PROPERTY_CONFIG_BUFFER_REQUIREMENTS:
			data_ptr += sizeof(u32);
			bufreq = (struct hfi_buffer_requirements *)data_ptr;
			event.buf_count =
				iris_hfi_gen1_bufreq_count_min(inst, bufreq);
			if (inst->core->iris_platform_data->legacy_vpu5)
				dev_info(inst->core->dev,
					 "Iris1 v64: HFI 4xx buffer minimum=%u (raw hold=%u min=%u)\n",
					 event.buf_count, bufreq->hold_count,
					 bufreq->count_min);
			data_ptr += sizeof(*bufreq);
			break;
		case HFI_INDEX_EXTRADATA_INPUT_CROP:
			data_ptr += sizeof(u32);
			crop = (struct hfi_extradata_input_crop *)data_ptr;
			event.input_crop.left = crop->left;
			event.input_crop.top = crop->top;
			event.input_crop.width = crop->width;
			event.input_crop.height = crop->height;
			data_ptr += sizeof(*crop);
			break;
		case HFI_PROPERTY_PARAM_VDEC_DPB_COUNTS:
			data_ptr += sizeof(u32);
			iris_vpu_dpb_count = (struct hfi_dpb_counts *)data_ptr;
			event.buf_count = iris_vpu_dpb_count->fw_min_count;
			data_ptr += sizeof(*iris_vpu_dpb_count);
			break;
		default:
			break;
		}
		num_properties_changed--;
	} while (num_properties_changed > 0);

	pixmp_ip->width = event.width;
	pixmp_ip->height = event.height;

	/* The bitstream, not the initial userspace hint, determines bit depth.
	 * Publish P010 with the source-change event so clients can allocate the
	 * correct CAPTURE layout before resuming a Main10/Profile 2 stream.
	 */
	if (core->iris_platform_data->legacy_vpu5 &&
	    event.bit_depth == HFI_BIT_DEPTH_10 &&
	    pixmp_op->pixelformat != V4L2_PIX_FMT_P010) {
		pixmp_op->pixelformat = V4L2_PIX_FMT_P010;
		inst->capture_format_changed = true;
	}

	pixmp_op->width = ALIGN(event.width, 128);
	pixmp_op->height = ALIGN(event.height, 32);
	pixmp_op->plane_fmt[0].bytesperline =
		iris_vdec_raw_stride(event.width, pixmp_op->pixelformat);
	pixmp_op->plane_fmt[0].sizeimage = iris_get_buffer_size(inst, BUF_OUTPUT);

	matrix_coeff =  FIELD_GET(GENMASK(7, 0), event.colour_space);
	transfer_char = FIELD_GET(GENMASK(15, 8), event.colour_space);
	primaries = FIELD_GET(GENMASK(23, 16), event.colour_space);
	colour_description_present_flag = FIELD_GET(GENMASK(24, 24), event.colour_space);
	full_range = FIELD_GET(GENMASK(25, 25), event.colour_space);
	video_signal_type_present_flag = FIELD_GET(GENMASK(29, 29), event.colour_space);

	pixmp_op->colorspace = V4L2_COLORSPACE_DEFAULT;
	pixmp_op->xfer_func = V4L2_XFER_FUNC_DEFAULT;
	pixmp_op->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	pixmp_op->quantization = V4L2_QUANTIZATION_DEFAULT;

	if (video_signal_type_present_flag) {
		pixmp_op->quantization =
			full_range ?
			V4L2_QUANTIZATION_FULL_RANGE :
			V4L2_QUANTIZATION_LIM_RANGE;
		if (colour_description_present_flag) {
			pixmp_op->colorspace =
				iris_hfi_get_v4l2_color_primaries(primaries);
			pixmp_op->xfer_func =
				iris_hfi_get_v4l2_transfer_char(transfer_char);
			pixmp_op->ycbcr_enc =
				iris_hfi_get_v4l2_matrix_coefficients(matrix_coeff);
		}
	}

	pixmp_ip->colorspace = pixmp_op->colorspace;
	pixmp_ip->xfer_func = pixmp_op->xfer_func;
	pixmp_ip->ycbcr_enc = pixmp_op->ycbcr_enc;
	pixmp_ip->quantization = pixmp_op->quantization;

	if (event.input_crop.width > 0 && event.input_crop.height > 0) {
		inst->crop.left = event.input_crop.left;
		inst->crop.top = event.input_crop.top;
		inst->crop.width = event.input_crop.width;
		inst->crop.height = event.input_crop.height;
	} else {
		inst->crop.left = 0;
		inst->crop.top = 0;
		inst->crop.width = event.width;
		inst->crop.height = event.height;
	}

	inst->fw_min_count = event.buf_count;
	inst->buffers[BUF_OUTPUT].min_count = iris_vpu_buf_count(inst, BUF_OUTPUT);
	inst->buffers[BUF_OUTPUT].size = pixmp_op->plane_fmt[0].sizeimage;
	ctrl = v4l2_ctrl_find(&inst->ctrl_handler, V4L2_CID_MIN_BUFFERS_FOR_CAPTURE);
	if (ctrl)
		v4l2_ctrl_s_ctrl(ctrl, inst->buffers[BUF_OUTPUT].min_count);

	dst_q = v4l2_m2m_get_dst_vq(inst->m2m_ctx);
	dst_q->min_reqbufs_allocation = inst->buffers[BUF_OUTPUT].min_count;

	if ((event.bit_depth != HFI_BIT_DEPTH_8 &&
	     event.bit_depth != HFI_BIT_DEPTH_10) ||
	    (event.bit_depth == HFI_BIT_DEPTH_10 &&
	     pixmp_op->pixelformat != V4L2_PIX_FMT_P010) ||
	    !event.pic_struct) {
		dev_err(core->dev, "unsupported content, bit depth: %x, pic_struct = %x\n",
			event.bit_depth, event.pic_struct);
		iris_inst_change_state(inst, IRIS_INST_ERROR);
	}
}

static void iris_hfi_gen1_event_seq_changed(struct iris_inst *inst,
					    struct hfi_msg_event_notify_pkt *pkt)
{
	u32 width = inst->fmt_dst->fmt.pix_mp.width;
	u32 height = inst->fmt_dst->fmt.pix_mp.height;
	u32 size = inst->buffers[BUF_OUTPUT].size;
	bool initial = !inst->fw_min_count;
	struct hfi_session_flush_pkt flush_pkt;
	u32 num_properties_changed;
	int ret;

	ret = iris_inst_sub_state_change_drc(inst);
	if (ret)
		return;

	switch (pkt->event_data1) {
	case HFI_EVENT_DATA_SEQUENCE_CHANGED_SUFFICIENT_BUF_RESOURCES:
	case HFI_EVENT_DATA_SEQUENCE_CHANGED_INSUFFICIENT_BUF_RESOURCES:
		break;
	default:
		dev_err(inst->core->dev, "invalid sequence-change resource status %#x\n",
			pkt->event_data1);
		iris_inst_change_state(inst, IRIS_INST_ERROR);
		return;
	}

	num_properties_changed = pkt->event_data2;
	if (!num_properties_changed) {
		dev_err(inst->core->dev, "sequence-change event has no properties\n");
		iris_inst_change_state(inst, IRIS_INST_ERROR);
		return;
	}

	iris_hfi_gen1_read_changed_params(inst, pkt);

	/* A client may start CAPTURE before the first sequence notification.
	 * If its allocation already fits, this is initialization, not a DRC
	 * drain. Flushing here manufactures LAST before the first picture and
	 * races clients which resume an unchanged format with DECODER_CMD_START.
	 */
	if (inst->core->iris_platform_data->legacy_vpu5 && initial &&
	    inst->state == IRIS_INST_STREAMING &&
	    width == inst->fmt_dst->fmt.pix_mp.width &&
	    height == inst->fmt_dst->fmt.pix_mp.height &&
	    size >= inst->buffers[BUF_OUTPUT].size &&
	    vb2_get_num_buffers(v4l2_m2m_get_dst_vq(inst->m2m_ctx)) >=
		inst->buffers[BUF_OUTPUT].min_count &&
	    inst->fw_min_count <= inst->buffers[BUF_DPB].min_count) {
		ret = inst->core->hfi_ops->session_resume_drc(inst,
						V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
		if (ret) {
			iris_inst_change_state(inst, IRIS_INST_ERROR);
			return;
		}
		iris_inst_change_sub_state(inst, IRIS_INST_SUB_DRC |
			IRIS_INST_SUB_FIRST_IPSC | IRIS_INST_SUB_INPUT_PAUSE, 0);
		iris_vdec_src_change(inst);
		return;
	}

	/*
	 * Drop the VP9 high-IOVA placeholder now that the sequence is known,
	 * so the top-down allocator hands the freed high region to the
	 * CAPTURE/DPB buffers userspace and the capture-path internal-buffer
	 * allocator are about to create.
	 */
	iris_vb2_vp9_release_high_iova_hole(inst);

	if (inst->state != IRIS_INST_ERROR && !(inst->sub_state & IRIS_INST_SUB_FIRST_IPSC)) {

		if (!inst->flush_responses_pending)
			reinit_completion(&inst->flush_completion);
		flush_pkt.shdr.hdr.size = sizeof(struct hfi_session_flush_pkt);
		flush_pkt.shdr.hdr.pkt_type = HFI_CMD_SESSION_FLUSH;
		flush_pkt.shdr.session_id = inst->session_id;
		flush_pkt.flush_type = HFI_FLUSH_OUTPUT;
		if (!iris_hfi_queue_cmd_write(inst->core, &flush_pkt, flush_pkt.shdr.hdr.size))
			inst->flush_responses_pending++;
	}

	iris_vdec_src_change(inst);
	iris_inst_sub_state_change_drc_last(inst);
}

static void
iris_hfi_gen1_sys_event_notify(struct iris_core *core, void *packet)
{
	struct hfi_msg_event_notify_pkt *pkt = packet;
	struct iris_inst *instance;

	/* An event for a closed or unknown session is not a core failure.
	 * Only an explicit system-error event may reset every live session.
	 */
	if (pkt->event_id != HFI_EVENT_SYS_ERROR) {
		dev_warn(core->dev,
			 "ignoring event for unknown session: event=%#x session=%#x data=%#x,%#x\n",
			 pkt->event_id, pkt->shdr.session_id,
			 pkt->event_data1, pkt->event_data2);
		return;
	}

	if (pkt->event_id == HFI_EVENT_SYS_ERROR) {
		u32 sfr_size;
		size_t message_len;
		char *message;

		dev_err(core->dev, "sys error (type: %x, session id:%x, data1:%x, data2:%x)\n",
			pkt->event_id, pkt->shdr.session_id, pkt->event_data1,
			pkt->event_data2);

		/*
		 * The delayed recovery worker frees the SFR allocation.  Copy its
		 * diagnostic to dmesg while the firmware-owned buffer is still
		 * valid, following the downstream Venus driver's failure path.
		 */
		if (core->sfr_vaddr) {
			dma_rmb();
			sfr_size = READ_ONCE(*(u32 *)core->sfr_vaddr);
			if (sfr_size > sizeof(u32) && sfr_size <= SZ_4K) {
				message = core->sfr_vaddr + sizeof(u32);
				message_len = strnlen(message,
						     sfr_size - sizeof(u32));
				dev_err(core->dev, "Iris1 v57 SFR: %.*s\n",
					(int)message_len, message);
			} else {
				dev_err(core->dev,
					"Iris1 v57 SFR has invalid size %u\n",
					sfr_size);
			}
		}
	}

	core->state = IRIS_CORE_ERROR;

	mutex_lock(&core->lock);
	list_for_each_entry(instance, &core->instances, list)
		iris_inst_change_state(instance, IRIS_INST_ERROR);
	mutex_unlock(&core->lock);

	schedule_delayed_work(&core->sys_error_handler, msecs_to_jiffies(10));
}

static void
iris_hfi_gen1_event_session_error(struct iris_inst *inst, struct hfi_msg_event_notify_pkt *pkt)
{
	switch (pkt->event_data1) {
	/* non fatal session errors */
	case HFI_ERR_SESSION_INVALID_SCALE_FACTOR:
	case HFI_ERR_SESSION_UNSUPPORT_BUFFERTYPE:
	case HFI_ERR_SESSION_UNSUPPORTED_SETTING:
	case HFI_ERR_SESSION_UPSCALE_NOT_SUPPORTED:
		dev_dbg(inst->core->dev, "session error: event id:%x, session id:%x\n",
			pkt->event_data1, pkt->shdr.session_id);
		break;
	/* fatal session errors */
	default:
		/*
		 * firmware fills event_data2 as an additional information about the
		 * hfi command for which session error has ouccured.
		 */
		dev_err(inst->core->dev,
			"session error for command: %x, event id:%x, session id:%x\n",
			pkt->event_data2, pkt->event_data1,
			pkt->shdr.session_id);
		iris_vb2_queue_error(inst);
		iris_inst_change_state(inst, IRIS_INST_ERROR);
		break;
	}
}

static void iris_hfi_gen1_session_event_notify(struct iris_inst *inst, void *packet)
{
	struct hfi_msg_event_notify_pkt *pkt = packet;

	switch (pkt->event_id) {
	case HFI_EVENT_SESSION_ERROR:
		iris_hfi_gen1_event_session_error(inst, pkt);
		break;
	case HFI_EVENT_SESSION_SEQUENCE_CHANGED:
		iris_hfi_gen1_event_seq_changed(inst, pkt);
		break;
	default:
		break;
	}
}

static void iris_hfi_gen1_sys_init_done(struct iris_core *core, void *packet)
{
	struct hfi_msg_sys_init_done_pkt *pkt = packet;

	if (pkt->error_type != HFI_ERR_NONE) {
		core->state = IRIS_CORE_ERROR;
		return;
	}

	complete(&core->core_init_done);
}

static void
iris_hfi_gen1_sys_get_prop_image_version(struct iris_core *core,
					 struct hfi_msg_sys_property_info_pkt *pkt)
{
	int req_bytes = pkt->hdr.size - sizeof(*pkt);
	char fw_version[IRIS_FW_VERSION_LENGTH];
	u8 *str_image_version;
	u32 i;

	if (req_bytes < IRIS_FW_VERSION_LENGTH - 1 || !pkt->data[0] || pkt->num_properties > 1) {
		dev_err(core->dev, "bad packet\n");
		return;
	}

	str_image_version = pkt->data;
	if (!str_image_version) {
		dev_err(core->dev, "firmware version not available\n");
		return;
	}

	for (i = 0; i < IRIS_FW_VERSION_LENGTH - 1; i++) {
		if (str_image_version[i] != '\0')
			fw_version[i] = str_image_version[i];
		else
			fw_version[i] = ' ';
	}
	fw_version[i] = '\0';
	dev_dbg(core->dev, "firmware version: %s\n", fw_version);
}

static void iris_hfi_gen1_sys_property_info(struct iris_core *core, void *packet)
{
	struct hfi_msg_sys_property_info_pkt *pkt = packet;

	if (!pkt->num_properties) {
		dev_dbg(core->dev, "no properties\n");
		return;
	}

	switch (pkt->property) {
	case HFI_PROPERTY_SYS_IMAGE_VERSION:
		iris_hfi_gen1_sys_get_prop_image_version(core, pkt);
		break;
	default:
		dev_dbg(core->dev, "unknown property data\n");
		break;
	}
}

static void iris_hfi_gen1_session_etb_done(struct iris_inst *inst, void *packet)
{
	struct hfi_msg_session_empty_buffer_done_pkt *pkt = packet;
	struct v4l2_m2m_ctx *m2m_ctx = inst->m2m_ctx;
	struct v4l2_m2m_buffer *m2m_buffer, *n;
	struct iris_buffer *buf = NULL;
	bool found = false;

	/*
	 * The EOS buffer sent by session_drain() is synthetic and is not in
	 * the V4L2 source buffer list. H.264/HEVC use address zero while VP9
	 * uses the legacy sentinel address.
	 */
	if (!pkt->packet_buffer || pkt->packet_buffer == 0xdeadb000) {
		dev_dbg(inst->core->dev,
			 "Iris1 ETB done: ignoring drain EOS tag=%u packet=%#x error=%#x\n",
			 pkt->input_tag, pkt->packet_buffer, pkt->shdr.error_type);
		return;
	}

	v4l2_m2m_for_each_src_buf_safe(m2m_ctx, m2m_buffer, n) {
		buf = to_iris_buffer(&m2m_buffer->vb);
		if (buf->index == pkt->input_tag) {
			found = true;
			break;
		}
	}
	if (!found)
		goto error;

	dev_dbg(inst->core->dev,
		 "Iris1 ETB done: tag=%u packet=%#x offset=%u filled=%u error=%#x\n",
		 pkt->input_tag, pkt->packet_buffer, pkt->offset, pkt->filled_len,
		 pkt->shdr.error_type);

	if (pkt->shdr.error_type == HFI_ERR_SESSION_UNSUPPORTED_STREAM) {
		dev_err(inst->core->dev,
			"firmware rejected %p4cc stream: input tag=%u error=%#x (format hint %ux%u)\n",
			&inst->codec, pkt->input_tag, pkt->shdr.error_type,
			inst->fmt_src->fmt.pix_mp.width, inst->fmt_src->fmt.pix_mp.height);
		buf->flags = V4L2_BUF_FLAG_ERROR;
		iris_vb2_queue_error(inst);
		iris_inst_change_state(inst, IRIS_INST_ERROR);
	}

	if (!(buf->attr & BUF_ATTR_QUEUED))
		return;

	buf->attr &= ~BUF_ATTR_QUEUED;

	if (!(buf->attr & BUF_ATTR_BUFFER_DONE)) {
		buf->attr |= BUF_ATTR_BUFFER_DONE;
		iris_vb2_buffer_done(inst, buf);
	}

	return;

error:
	iris_inst_change_state(inst, IRIS_INST_ERROR);
	dev_err(inst->core->dev,
		"Iris1 ETB done unmatched: tag=%u packet=%#x offset=%u filled=%u error=%#x\n",
		pkt->input_tag, pkt->packet_buffer, pkt->offset, pkt->filled_len,
		pkt->shdr.error_type);
}

static void iris_hfi_gen1_session_ftb_done(struct iris_inst *inst, void *packet)
{
	struct hfi_msg_session_fbd_uncompressed_plane0_pkt *uncom_pkt = packet;
	struct hfi_msg_session_fbd_compressed_pkt *com_pkt = packet;
	struct v4l2_m2m_ctx *m2m_ctx = inst->m2m_ctx;
	struct v4l2_m2m_buffer *m2m_buffer, *n;
	struct hfi_session_flush_pkt flush_pkt;
	u32 timestamp_hi;
	u32 timestamp_lo;
	struct iris_core *core = inst->core;
	u32 filled_len;
	u32 pic_type;
	u32 output_tag;
	struct iris_buffer *buf, *iter;
	struct iris_buffers *buffers;
	u32 hfi_flags;
	u32 offset;
	u64 timestamp_us = 0;
	bool m2m_stopped;
	bool recycle_output = false;
	bool seek_was_pending = false;
	bool found = false;
	u32 flags = 0;
	int ret;

	if (inst->domain == DECODER) {
		timestamp_hi = uncom_pkt->time_stamp_hi;
		timestamp_lo = uncom_pkt->time_stamp_lo;
		filled_len = uncom_pkt->filled_len;
		pic_type = uncom_pkt->picture_type;
		output_tag = uncom_pkt->output_tag;
		hfi_flags = uncom_pkt->flags;
		offset = uncom_pkt->offset;
	} else {
		timestamp_hi = com_pkt->time_stamp_hi;
		timestamp_lo = com_pkt->time_stamp_lo;
		filled_len = com_pkt->filled_len;
		pic_type = com_pkt->picture_type;
		output_tag = com_pkt->output_tag;
		hfi_flags = com_pkt->flags;
		offset = com_pkt->offset;
	}

	if ((hfi_flags & HFI_BUFFERFLAG_EOS) && !filled_len) {
		reinit_completion(&inst->flush_completion);

		flush_pkt.shdr.hdr.size = sizeof(struct hfi_session_flush_pkt);
		flush_pkt.shdr.hdr.pkt_type = HFI_CMD_SESSION_FLUSH;
		flush_pkt.shdr.session_id = inst->session_id;
		flush_pkt.flush_type = HFI_FLUSH_OUTPUT;
		if (!iris_hfi_queue_cmd_write(core, &flush_pkt, flush_pkt.shdr.hdr.size))
			inst->flush_responses_pending++;

		iris_inst_sub_state_change_drain_last(inst);
	}

	if (iris_split_mode_enabled(inst) && inst->domain == DECODER &&
	    uncom_pkt->stream_id == 0) {
		buffers = &inst->buffers[BUF_DPB];
		if (!buffers)
			goto error;

		found = false;
		list_for_each_entry(iter, &buffers->list, list) {
			if (!(iter->attr & BUF_ATTR_QUEUED))
				continue;

			found = (iter->index == output_tag &&
				iter->data_offset == offset);

			if (found) {
				buf = iter;
				break;
			}
		}
	} else {
		v4l2_m2m_for_each_dst_buf_safe(m2m_ctx, m2m_buffer, n) {
			buf = to_iris_buffer(&m2m_buffer->vb);
			if (!(buf->attr & BUF_ATTR_QUEUED))
				continue;

			found = (buf->index == output_tag &&
				 buf->data_offset == offset);

			if (found)
				break;
		}
	}
	if (!found)
		goto error;

	buf->data_offset = offset;
	buf->data_size = filled_len;
	if (inst->domain == DECODER && buf->type == BUF_OUTPUT && filled_len &&
	    inst->codec == V4L2_PIX_FMT_VP9 &&
	    (uncom_pkt->frame_width != inst->crop.width ||
	     uncom_pkt->frame_height != inst->crop.height))
		dev_info_ratelimited(core->dev,
			"VP9 output geometry: frame=%ux%u origin=%u,%u crop=%ux%u capture=%ux%u tag=%u flags=%#x\n",
			uncom_pkt->frame_width, uncom_pkt->frame_height,
			uncom_pkt->start_x_coord, uncom_pkt->start_y_coord,
			inst->crop.width, inst->crop.height,
			inst->fmt_dst->fmt.pix_mp.width, inst->fmt_dst->fmt.pix_mp.height,
			output_tag, hfi_flags);

	if (filled_len) {
		timestamp_us = timestamp_hi;
		timestamp_us = (timestamp_us << 32) | timestamp_lo;
	} else {
		if (inst->domain == DECODER && uncom_pkt->stream_id == 1 &&
		    !inst->last_buffer_dequeued) {
			if (iris_drc_pending(inst) || iris_drain_pending(inst)) {
				flags |= V4L2_BUF_FLAG_LAST;
				inst->last_buffer_dequeued = true;
			}
		} else if (inst->domain == ENCODER) {
			if (!inst->last_buffer_dequeued && iris_drain_pending(inst)) {
				flags |= V4L2_BUF_FLAG_LAST;
				inst->last_buffer_dequeued = true;
			}
		}
	}
	buf->timestamp = timestamp_us;

	if (inst->domain == DECODER && buf->type == BUF_OUTPUT &&
	    inst->core->iris_platform_data->legacy_vpu5) {
		/* Legacy VPU5 emits both subframes from a VP9 superframe, including
		 * the leading show_frame=0 alt-ref frame.  HFI gives both outputs the
		 * packet timestamp and does not set DROP_FRAME.  Keep one output
		 * pending so a same-timestamp successor can replace the hidden frame. */
		if (inst->codec == V4L2_PIX_FMT_VP9 && filled_len &&
		    inst->pending_output &&
		    inst->pending_output->timestamp == timestamp_us)
			ret = iris_vdec_recycle_pending_output(inst);
		else
			ret = iris_vdec_complete_pending_output(inst);
		if (ret)
			goto error;
		seek_was_pending = inst->seek_timestamp_pending;
		if (inst->seek_hold_frames)
			inst->seek_hold_frames--;
	}

	if (inst->domain == DECODER && buf->type == BUF_OUTPUT && filled_len) {
		/* Legacy VP9 reports DROP_FRAME on filled, otherwise usable output
		 * around show-existing frames and immediately after an in-place seek.
		 * Hiding that completion starves one userspace target per occurrence
		 * and can exhaust Chromium's pool.  Complete those VP9 buffers below;
		 * retain the bounded recycle workaround for the other codecs. */
		if (inst->core->iris_platform_data->legacy_vpu5 &&
		    inst->codec != V4L2_PIX_FMT_VP9 &&
		    (hfi_flags & (HFI_BUFFERFLAG_DATACORRUPT |
				  HFI_BUFFERFLAG_DROP_FRAME)) &&
		    inst->corrupt_output_drops < IRIS1_MAX_CORRUPT_OUTPUT_DROPS) {
			inst->corrupt_output_drops++;
			recycle_output = true;
			dev_info(inst->core->dev,
				 "Iris1 v137: recycling corrupt output timestamp %llu ns flags %#x (drop %u/%u)\n",
				 timestamp_us, hfi_flags,
				 inst->corrupt_output_drops,
				 IRIS1_MAX_CORRUPT_OUTPUT_DROPS);
		} else {
			inst->corrupt_output_drops = 0;
			recycle_output = iris_vdec_discard_stale_frame(inst,
								timestamp_us);
		}
	}

	if (recycle_output) {
		buf->data_size = 0;
		buf->flags = 0;
		buf->timestamp = 0;
		buf->attr &= ~(BUF_ATTR_QUEUED | BUF_ATTR_DEQUEUED |
			       BUF_ATTR_BUFFER_DONE);
		ret = iris_queue_buffer(inst, buf);
		if (ret)
			goto error;
		return;
	}

	switch (pic_type) {
	case HFI_GEN1_PICTURE_IDR:
	case HFI_GEN1_PICTURE_I:
		flags |= V4L2_BUF_FLAG_KEYFRAME;
		break;
	case HFI_GEN1_PICTURE_P:
		flags |= V4L2_BUF_FLAG_PFRAME;
		break;
	case HFI_GEN1_PICTURE_B:
		flags |= V4L2_BUF_FLAG_BFRAME;
		break;
	case HFI_FRAME_NOTCODED:
	case HFI_UNUSED_PICT:
	case HFI_FRAME_YUV:
	default:
		break;
	}

	buf->attr &= ~BUF_ATTR_QUEUED;
	buf->attr |= BUF_ATTR_DEQUEUED;
	buf->attr |= BUF_ATTR_BUFFER_DONE;

	/* On legacy VP9, filled DROP_FRAME/DATACORRUPT buffers are usable output.
	 * Do not turn them into VB2 errors: iris_vb2_buffer_done() would discard
	 * their payload, and Chromium treats the dequeue error as fatal. */
	if ((hfi_flags & HFI_BUFFERFLAG_DATACORRUPT) &&
	    !(inst->domain == DECODER && buf->type == BUF_OUTPUT && filled_len &&
	      inst->core->iris_platform_data->legacy_vpu5 &&
	      inst->codec == V4L2_PIX_FMT_VP9))
		flags |= V4L2_BUF_FLAG_ERROR;

	if ((hfi_flags & HFI_BUFFERFLAG_DROP_FRAME) &&
	    !(inst->domain == DECODER && buf->type == BUF_OUTPUT && filled_len &&
	      inst->core->iris_platform_data->legacy_vpu5 &&
	      inst->codec == V4L2_PIX_FMT_VP9))
		flags |= V4L2_BUF_FLAG_ERROR;

	buf->flags |= flags;

	if (inst->domain == DECODER && buf->type == BUF_OUTPUT && filled_len &&
	    inst->core->iris_platform_data->legacy_vpu5 &&
	    !seek_was_pending &&
	    (inst->codec == V4L2_PIX_FMT_VP9 ||
	     (!(hfi_flags & (HFI_BUFFERFLAG_DATACORRUPT |
			     HFI_BUFFERFLAG_DROP_FRAME)) &&
	      inst->seek_hold_frames))) {
		ret = iris_vdec_hold_output(inst, buf);
		if (ret)
			goto error;
		return;
	}

	iris_vb2_buffer_done(inst, buf);

	return;

error:
	/*
	 * A terminal LAST and its final FTB done can be adjacent in the response
	 * queue, so the context may already be stopped with the buffer off the
	 * destination list.  Keep unmatched FTBs fatal in every other state so a
	 * genuine playback failure is not hidden.
	 */
	m2m_stopped = v4l2_m2m_has_stopped(m2m_ctx);
	if (inst->streamoff_pending || inst->last_buffer_dequeued || m2m_stopped) {
		dev_info(core->dev,
			 "Iris1 v99: ignoring terminal late FTB: tag=%u offset=%u filled=%u flags=%#x state=%u substate=%#x streamoff=%u last=%u stopped=%u\n",
			 output_tag, offset, filled_len, hfi_flags, inst->state,
			 inst->sub_state, inst->streamoff_pending,
			 inst->last_buffer_dequeued, m2m_stopped);
		return;
	}

	dev_err(core->dev,
		"Iris1 v99: FTB done unmatched: tag=%u offset=%u filled=%u flags=%#x state=%u substate=%#x\n",
		output_tag, offset, filled_len, hfi_flags, inst->state,
		inst->sub_state);
	iris_inst_change_state(inst, IRIS_INST_ERROR);
}

static void
iris_hfi_gen1_session_property_info(struct iris_inst *inst, void *packet)
{
	struct hfi_msg_session_property_info_pkt *pkt = packet;
	struct hfi_buffer_requirements *req;
	enum iris_buffer_type buffer_type;
	u32 req_bytes, count, i;

	req_bytes = pkt->shdr.hdr.size - sizeof(*pkt);
	dev_info(inst->core->dev,
		 "Iris1 v58: SESSION_PROPERTY_INFO properties=%u property=%#x payload=%u bytes\n",
		 pkt->num_properties, pkt->property, req_bytes);

	if (pkt->num_properties != 1 ||
	    pkt->property != HFI_PROPERTY_CONFIG_BUFFER_REQUIREMENTS ||
	    !req_bytes || req_bytes % sizeof(*req)) {
		dev_err(inst->core->dev,
			"Iris1 v58: malformed buffer requirements response\n");
		complete(&inst->completion);
		return;
	}

	count = req_bytes / sizeof(*req);
	req = (struct hfi_buffer_requirements *)pkt->data;
	for (i = 0; i < count; i++, req++) {
		dev_info(inst->core->dev,
			 "Iris1 v58: bufreq[%u] type=%#x size=%u region=%u hold=%u min=%u actual=%u contiguous=%u align=%u\n",
			 i, req->type, req->size, req->region_size,
			 req->hold_count, req->count_min, req->count_actual,
			 req->contiguous, req->alignment);

		switch (req->type) {
		case HFI_BUFFER_OUTPUT:
			/*
			 * In decoder split mode OUTPUT is the firmware-owned UBWC
			 * DPB.  VIDEO.IR.1.2 reports its exact allocation size and
			 * validates BUFFER_SIZE_ACTUAL strictly at SESSION_CONTINUE;
			 * the generic layout helper includes newer-HFI tail padding
			 * and can therefore be larger than the VPU5 requirement.
			 */
			if (inst->domain != DECODER ||
			    !iris_split_mode_enabled(inst))
				continue;
			buffer_type = BUF_DPB;
			break;
		case HFI_BUFFER_INTERNAL_PERSIST_1:
			buffer_type = BUF_PERSIST;
			break;
		case HFI_BUFFER_INTERNAL_SCRATCH:
			buffer_type = BUF_BIN;
			break;
		case HFI_BUFFER_INTERNAL_SCRATCH_1:
			buffer_type = BUF_SCRATCH_1;
			break;
		default:
			continue;
		}

		inst->fw_buffer_sizes[buffer_type] =
			max(inst->fw_buffer_sizes[buffer_type], req->size);
	}

	complete(&inst->completion);
}

struct iris_hfi_gen1_response_pkt_info {
	u32 pkt;
	u32 pkt_sz;
};

static const struct iris_hfi_gen1_response_pkt_info pkt_infos[] = {
	{
	 .pkt = HFI_MSG_EVENT_NOTIFY,
	 .pkt_sz = sizeof(struct hfi_msg_event_notify_pkt),
	},
	{
	 .pkt = HFI_MSG_SYS_INIT,
	 .pkt_sz = sizeof(struct hfi_msg_sys_init_done_pkt),
	},
	{
	 .pkt = HFI_MSG_SYS_PROPERTY_INFO,
	 .pkt_sz = sizeof(struct hfi_msg_sys_property_info_pkt),
	},
	{
	 .pkt = HFI_MSG_SYS_SESSION_INIT,
	 .pkt_sz = sizeof(struct hfi_msg_session_init_done_pkt),
	},
	{
	 .pkt = HFI_MSG_SYS_SESSION_END,
	 .pkt_sz = sizeof(struct hfi_msg_session_hdr_pkt),
	},
	{
	 .pkt = HFI_MSG_SESSION_LOAD_RESOURCES,
	 .pkt_sz = sizeof(struct hfi_msg_session_hdr_pkt),
	},
	{
	 .pkt = HFI_MSG_SESSION_START,
	 .pkt_sz = sizeof(struct hfi_msg_session_hdr_pkt),
	},
	{
	 .pkt = HFI_MSG_SESSION_STOP,
	 .pkt_sz = sizeof(struct hfi_msg_session_hdr_pkt),
	},
	{
	 .pkt = HFI_MSG_SESSION_EMPTY_BUFFER,
	 .pkt_sz = sizeof(struct hfi_msg_session_empty_buffer_done_pkt),
	},
	{
	 .pkt = HFI_MSG_SESSION_FILL_BUFFER,
	 .pkt_sz = sizeof(struct hfi_msg_session_fbd_compressed_pkt),
	},
	{
	 .pkt = HFI_MSG_SESSION_PROPERTY_INFO,
	 .pkt_sz = sizeof(struct hfi_msg_session_property_info_pkt),
	},
	{
	 .pkt = HFI_MSG_SESSION_FLUSH,
	 .pkt_sz = sizeof(struct hfi_msg_session_flush_done_pkt),
	},
	{
	 .pkt = HFI_MSG_SESSION_RELEASE_RESOURCES,
	 .pkt_sz = sizeof(struct hfi_msg_session_hdr_pkt),
	},
	{
	 .pkt = HFI_MSG_SESSION_RELEASE_BUFFERS,
	 .pkt_sz = sizeof(struct hfi_msg_session_release_buffers_done_pkt),
	},
};

static void iris_hfi_gen1_handle_response(struct iris_core *core, void *response)
{
	struct hfi_pkt_hdr *hdr = (struct hfi_pkt_hdr *)response;
	const struct iris_hfi_gen1_response_pkt_info *pkt_info;
	struct device *dev = core->dev;
	struct hfi_session_pkt *pkt;
	struct iris_inst *inst;
	bool found = false;
	u32 i;

	for (i = 0; i < ARRAY_SIZE(pkt_infos); i++) {
		pkt_info = &pkt_infos[i];
		if (pkt_info->pkt != hdr->pkt_type)
			continue;
		found = true;
		break;
	}

	if (!found || hdr->size < pkt_info->pkt_sz) {
		dev_err(dev, "bad packet size (%d should be %d, pkt type:%x, found %d)\n",
			hdr->size, pkt_info->pkt_sz, hdr->pkt_type, found);

		return;
	}

	switch (hdr->pkt_type) {
	case HFI_MSG_SYS_INIT:
		iris_hfi_gen1_sys_init_done(core, hdr);
		break;
	case HFI_MSG_SYS_PROPERTY_INFO:
		iris_hfi_gen1_sys_property_info(core, hdr);
		break;
	case HFI_MSG_EVENT_NOTIFY:
		pkt = (struct hfi_session_pkt *)hdr;
		inst = iris_get_instance(core, pkt->shdr.session_id);
		if (inst) {
			mutex_lock(&inst->lock);
			iris_hfi_gen1_session_event_notify(inst, hdr);
			mutex_unlock(&inst->lock);
		} else {
			iris_hfi_gen1_sys_event_notify(core, hdr);
		}

		break;
	default:
		pkt = (struct hfi_session_pkt *)hdr;
		inst = iris_get_instance(core, pkt->shdr.session_id);
		if (!inst) {
			dev_warn(dev, "no valid instance(pkt session_id:%x, pkt:%x)\n",
				 pkt->shdr.session_id,
				 pkt_info ? pkt_info->pkt : 0);
			return;
		}

		mutex_lock(&inst->lock);
		if (hdr->pkt_type == HFI_MSG_SESSION_EMPTY_BUFFER) {
			iris_hfi_gen1_session_etb_done(inst, hdr);
		} else if (hdr->pkt_type == HFI_MSG_SESSION_FILL_BUFFER) {
			iris_hfi_gen1_session_ftb_done(inst, hdr);
		} else if (hdr->pkt_type == HFI_MSG_SESSION_PROPERTY_INFO) {
			iris_hfi_gen1_session_property_info(inst, hdr);
		} else {
			struct hfi_msg_session_hdr_pkt *shdr;

			shdr = (struct hfi_msg_session_hdr_pkt *)hdr;
			dev_dbg(dev,
				 "Iris1 response: type=%#x size=%u session=%#x error=%#x\n",
				 hdr->pkt_type, hdr->size, shdr->shdr.session_id,
				 shdr->error_type);
			/*
			 * HFI teardown is idempotent.  SM8150 can report STOP as already
			 * outside START after drain/flush, followed by RELEASE_RESOURCES
			 * as already released.  Accept only these command/state-specific
			 * statuses; the same errors remain fatal during normal streaming.
			 */
			if (shdr->error_type == HFI_ERR_SESSION_SAME_STATE_OPERATION &&
			    inst->streamoff_pending) {
				dev_dbg(dev,
					 "Iris1 v99: idempotent response type=%#x error=%#x\n",
					 hdr->pkt_type, shdr->error_type);
			} else if (hdr->pkt_type == HFI_MSG_SESSION_STOP &&
				   shdr->error_type ==
					   HFI_ERR_SESSION_INCORRECT_STATE_OPERATION &&
				   inst->streamoff_pending) {
				dev_dbg(dev,
					 "Iris1 v99: STOP already complete during streamoff\n");
			} else if (shdr->error_type != HFI_ERR_NONE) {
				dev_err(dev,
					"Iris1 response error: type=%#x session=%#x error=%#x\n",
					hdr->pkt_type, shdr->shdr.session_id,
					shdr->error_type);
				iris_inst_change_state(inst, IRIS_INST_ERROR);
			}

			if (pkt_info->pkt == HFI_MSG_SESSION_FLUSH) {
				if (!(--inst->flush_responses_pending))
					complete(&inst->flush_completion);
			} else {
				complete(&inst->completion);
			}
		}
		mutex_unlock(&inst->lock);

		break;
	}
}

static void iris_hfi_gen1_flush_debug_queue(struct iris_core *core, u8 *packet)
{
	struct hfi_msg_sys_coverage_pkt *pkt;

	while (!iris_hfi_queue_dbg_read(core, packet)) {
		pkt = (struct hfi_msg_sys_coverage_pkt *)packet;

		if (pkt->hdr.pkt_type != HFI_MSG_SYS_COV) {
			struct hfi_msg_sys_debug_pkt *pkt =
				(struct hfi_msg_sys_debug_pkt *)packet;
			u32 msg_size;

			/*
			 * VPU5 reports the useful reason for a deferred session
			 * failure on the debug queue. Keep it visible while
			 * bringing up SM8150: dev_dbg() consumes the packet but
			 * normally hides it unless dynamic debug was enabled
			 * before the interrupt arrived.
			 */
			if (pkt->hdr.size <= sizeof(*pkt))
				continue;
			msg_size = min_t(u32, pkt->msg_size,
					 pkt->hdr.size - sizeof(*pkt));
			dev_dbg(core->dev, "Iris1 FW: %.*s",
				 (int)msg_size, pkt->msg_data);
		}
	}
}

static void iris_hfi_gen1_response_handler(struct iris_core *core)
{
	memset(core->response_packet, 0, sizeof(struct hfi_pkt_hdr));
	while (!iris_hfi_queue_msg_read(core, core->response_packet)) {
		iris_hfi_gen1_handle_response(core, core->response_packet);
		memset(core->response_packet, 0, sizeof(struct hfi_pkt_hdr));
	}

	iris_hfi_gen1_flush_debug_queue(core, core->response_packet);
}

static const struct iris_hfi_response_ops iris_hfi_gen1_response_ops = {
	.hfi_response_handler = iris_hfi_gen1_response_handler,
};

void iris_hfi_gen1_response_ops_init(struct iris_core *core)
{
	core->hfi_response_ops = &iris_hfi_gen1_response_ops;
}
