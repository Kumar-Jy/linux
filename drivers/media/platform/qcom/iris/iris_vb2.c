// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <media/videobuf2-dma-contig.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>

#include "iris_common.h"
#include "iris_instance.h"
#include "iris_vb2.h"
#include "iris_vdec.h"
#include "iris_venc.h"
#include "iris_power.h"

/*
 * SM8150 Venus validates OUTPUT/OUTPUT2 IOVAs against a boundary near the
 * top of the non-secure aperture (~0xd0000000).  FFmpeg's m2m flow allocates
 * the bitstream and firmware-internal buffers before CAPTURE, consuming the
 * high IOVAs those OUTPUT buffers need.  Reserve the high region with a
 * throwaway placeholder during input setup, then release it right before the
 * CAPTURE queue is allocated so the top-down iommu-dma allocator hands the
 * freed high region to the output buffers.
 */
#define IRIS1_VP9_IOVA_HOLE_SIZE	0x04000000ULL	/* 64 MiB placeholder */

static bool cached_capture;
module_param(cached_capture, bool, 0444);
MODULE_PARM_DESC(cached_capture,
		 "Use cacheable H.264/HEVC/VP9 CAPTURE MMAP buffers on legacy Iris1");

static bool iris1_use_cached_capture(struct iris_inst *inst,
				     struct vb2_queue *q)
{
	return cached_capture &&
	       inst->core->iris_platform_data->legacy_vpu5 &&
	       (inst->codec == V4L2_PIX_FMT_H264 ||
		inst->codec == V4L2_PIX_FMT_HEVC ||
		inst->codec == V4L2_PIX_FMT_VP9) &&
	       V4L2_TYPE_IS_CAPTURE(q->type) &&
	       q->memory == VB2_MEMORY_MMAP;
}

static bool iris1_vp9_needs_iova_hole(struct iris_inst *inst)
{
	return inst->core->iris_platform_data->legacy_vpu5 &&
	       inst->codec == V4L2_PIX_FMT_VP9;
}

int iris_vb2_vp9_alloc_high_iova_hole(struct iris_inst *inst)
{
	struct iris_core *core = inst->core;
	size_t size = IRIS1_VP9_IOVA_HOLE_SIZE;
	dma_addr_t daddr;
	void *vaddr;

	if (!iris1_vp9_needs_iova_hole(inst) || inst->vp9_iova_hole_size)
		return 0;

	vaddr = dma_alloc_attrs(core->dev, size, &daddr, GFP_KERNEL,
			       DMA_ATTR_WRITE_COMBINE);
	if (!vaddr) {
		dev_err(core->dev,
			"Iris1 v85: VP9 IOVA hole alloc of %#zx failed\n",
			size);
		return -ENOMEM;
	}

	inst->vp9_iova_hole_vaddr = vaddr;
	inst->vp9_iova_hole_daddr = daddr;
	inst->vp9_iova_hole_size = size;

	dev_info(core->dev,
		 "Iris1 v85: VP9 reserved high IOVA hole %#llx-%#llx (size %#zx)\n",
		 (unsigned long long)daddr,
		 (unsigned long long)daddr + size - 1, size);

	return 0;
}

void iris_vb2_vp9_release_high_iova_hole(struct iris_inst *inst)
{
	struct iris_core *core = inst->core;
	dma_addr_t daddr;
	void *vaddr;
	size_t size;

	if (!inst->vp9_iova_hole_size)
		return;

	vaddr = inst->vp9_iova_hole_vaddr;
	daddr = inst->vp9_iova_hole_daddr;
	size = inst->vp9_iova_hole_size;

	inst->vp9_iova_hole_vaddr = NULL;
	inst->vp9_iova_hole_daddr = 0;
	inst->vp9_iova_hole_size = 0;

	dma_free_attrs(core->dev, size, vaddr, daddr, DMA_ATTR_WRITE_COMBINE);

	dev_info(core->dev,
		 "Iris1 v85: VP9 released high IOVA hole %#llx-%#llx\n",
		 (unsigned long long)daddr,
		 (unsigned long long)daddr + size - 1);
}

static int iris_check_inst_mbpf(struct iris_inst *inst)
{
	struct platform_inst_caps *caps;
	u32 mbpf, max_mbpf;

	caps = inst->core->iris_platform_data->inst_caps;
	max_mbpf = caps->max_mbpf;
	mbpf = iris_get_mbpf(inst);
	if (mbpf > max_mbpf)
		return -ENOMEM;

	return 0;
}

static int iris_check_resolution_supported(struct iris_inst *inst)
{
	u32 width, height, min_width, min_height, max_width, max_height;
	struct platform_inst_caps *caps;

	caps = inst->core->iris_platform_data->inst_caps;
	width = inst->fmt_src->fmt.pix_mp.width;
	height = inst->fmt_src->fmt.pix_mp.height;

	min_width = caps->min_frame_width;
	max_width = caps->max_frame_width;
	min_height = caps->min_frame_height;
	max_height = caps->max_frame_height;

	if (!(min_width <= width && width <= max_width) ||
	    !(min_height <= height && height <= max_height))
		return -EINVAL;

	return 0;
}

static int iris_check_session_supported(struct iris_inst *inst)
{
	struct iris_core *core = inst->core;
	struct iris_inst *instance = NULL;
	bool found = false;
	int ret;

	mutex_lock(&core->lock);
	list_for_each_entry(instance, &core->instances, list) {
		if (instance == inst)
			found = true;
	}
	mutex_unlock(&core->lock);

	if (!found) {
		ret = -EINVAL;
		goto exit;
	}

	ret = iris_check_core_mbpf(inst);
	if (ret)
		goto exit;

	ret = iris_check_inst_mbpf(inst);
	if (ret)
		goto exit;

	ret = iris_check_resolution_supported(inst);
	if (ret)
		goto exit;

	return 0;
exit:
	dev_err(inst->core->dev, "current session not supported(%d)\n", ret);

	return ret;
}

int iris_vb2_buf_init(struct vb2_buffer *vb2)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb2);
	struct iris_buffer *buf = to_iris_buffer(vbuf);

	buf->device_addr = vb2_dma_contig_plane_dma_addr(vb2, 0);

	return 0;
}

int iris_vb2_queue_setup(struct vb2_queue *q,
			 unsigned int *num_buffers, unsigned int *num_planes,
			 unsigned int sizes[], struct device *alloc_devs[])
{
	struct iris_inst *inst;
	struct iris_core *core;
	struct v4l2_format *f;
	u32 admission_fps;
	int ret = 0;

	inst = vb2_get_drv_priv(q);

	mutex_lock(&inst->lock);
	if (inst->state == IRIS_INST_ERROR) {
		dev_err_ratelimited(inst->core->dev,
				    "queue setup rejected: instance is in error state (queue type %u)\n",
				    q->type);
		ret = -EBUSY;
		goto unlock;
	}

	core = inst->core;
	f = V4L2_TYPE_IS_OUTPUT(q->type) ? inst->fmt_src : inst->fmt_dst;

	/*
	 * FFmpeg's v4l2m2m-copy path maps CAPTURE buffers into the CPU and
	 * uploads every decoded frame to the display API.  Coherent DMA memory
	 * is expensive to read on SM8150, especially for 4K60 NV12.  For this
	 * opt-in Iris1 decode path, allocate cacheable streaming DMA memory
	 * and let vb2_dma_contig perform the required post-DMA cache maintenance
	 * before each completed frame is returned to userspace.
	 *
	 * Keep this limited to MMAP: imported DMABUF ownership and cache policy
	 * belong to the exporter.  VP9 keeps its high-IOVA placeholder lifecycle:
	 * the placeholder is released before these CAPTURE buffers are allocated.
	 */
	if (iris1_use_cached_capture(inst, q)) {
		q->dma_dir = DMA_FROM_DEVICE;
		q->non_coherent_mem = true;
		dev_info(core->dev,
			 "Iris1 v100: using cacheable %s CAPTURE MMAP buffers; dynamic power vote starts at %u fps\n",
			 inst->codec == V4L2_PIX_FMT_H264 ? "H.264" :
			 inst->codec == V4L2_PIX_FMT_HEVC ? "HEVC" : "VP9",
			 iris_get_operating_fps(inst));
	}

	if (*num_planes) {
		if (*num_planes != f->fmt.pix_mp.num_planes ||
		    sizes[0] < f->fmt.pix_mp.plane_fmt[0].sizeimage)
			ret = -EINVAL;
		goto unlock;
	}
	if (V4L2_TYPE_IS_CAPTURE(q->type) && inst->core_load_rejected) {
		ret = -ENOMEM;
		goto unlock;
	}

	ret = iris_check_session_supported(inst);
	if (ret)
		goto unlock;

	ret = iris_hfi_session_open(inst);
	if (ret) {
		dev_err(core->dev, "session open failed: %d\n", ret);
		goto unlock;
	}

	if (V4L2_TYPE_IS_CAPTURE(q->type) && !inst->core_load_reserved) {
		if (inst->domain == DECODER)
			admission_fps = iris_vdec_get_admission_fps(inst);
		else
			admission_fps = max(inst->frame_rate,
					    inst->operating_rate);

		ret = iris_reserve_core_load(inst, admission_fps);
		if (ret) {
			inst->core_load_rejected = true;
			goto unlock;
		}
	}

	/*
	 * Hold the high IOVA region across input-side allocations so the
	 * top-down allocator returns high IOVAs to the CAPTURE/DPB buffers
	 * queued later.  Allocate the placeholder when the OUTPUT queue is
	 * set up (before bitstream buffers are allocated) and release it on
	 * the first CAPTURE queue_setup so the released high region is the
	 * next IOVA handed out by the allocator.
	 */
	if (V4L2_TYPE_IS_OUTPUT(q->type)) {
		ret = iris_vb2_vp9_alloc_high_iova_hole(inst);
		if (ret)
			goto unlock;
	} else if (V4L2_TYPE_IS_CAPTURE(q->type)) {
		iris_vb2_vp9_release_high_iova_hole(inst);
	}

	*num_planes = 1;
	sizes[0] = f->fmt.pix_mp.plane_fmt[0].sizeimage;

unlock:
	mutex_unlock(&inst->lock);

	return ret;
}

int iris_vb2_start_streaming(struct vb2_queue *q, unsigned int count)
{
	enum iris_buffer_type buf_type;
	struct iris_inst *inst;
	int ret = 0;

	inst = vb2_get_drv_priv(q);

	mutex_lock(&inst->lock);
	if (inst->state == IRIS_INST_ERROR) {
		ret = -EBUSY;
		goto error;
	}

	if (!V4L2_TYPE_IS_OUTPUT(q->type) &&
	    !V4L2_TYPE_IS_CAPTURE(q->type)) {
		ret = -EINVAL;
		goto error;
	}

	ret = iris_check_session_supported(inst);
	if (ret)
		goto error;

	ret = iris_hfi_session_open(inst);
	if (ret)
		goto error;

	iris_scale_power(inst);

	if (V4L2_TYPE_IS_OUTPUT(q->type)) {
		if (inst->domain == DECODER)
			ret = iris_vdec_streamon_input(inst);
		else
			ret = iris_venc_streamon_input(inst);
	} else if (V4L2_TYPE_IS_CAPTURE(q->type)) {
		if (inst->domain == DECODER)
			ret = iris_vdec_streamon_output(inst);
		else
			ret = iris_venc_streamon_output(inst);
	}
	if (ret) {
		dev_err(inst->core->dev,
			"Iris1 v119: start_streaming failed queue=%u count=%u state=%u ret=%d\n",
			q->type, count, inst->state, ret);
		goto error;
	}

	buf_type = iris_v4l2_type_to_driver(q->type);

	if (inst->domain == DECODER) {
		if (buf_type == BUF_INPUT)
			ret = iris_queue_deferred_buffers(inst, BUF_INPUT);

		if (!ret && inst->state == IRIS_INST_STREAMING) {
			ret = iris_queue_internal_deferred_buffers(inst, BUF_DPB);
			if (!ret)
				ret = iris_queue_deferred_buffers(inst, BUF_OUTPUT);
		}
	} else {
		if (inst->state == IRIS_INST_STREAMING) {
			ret = iris_queue_deferred_buffers(inst, BUF_INPUT);
			if (!ret)
				ret = iris_queue_deferred_buffers(inst, BUF_OUTPUT);
		}
	}

	if (ret) {
		dev_err(inst->core->dev,
			"Iris1 v119: deferred buffer queue failed queue=%u count=%u state=%u ret=%d\n",
			q->type, count, inst->state, ret);
		goto error;
	}

	mutex_unlock(&inst->lock);

	return ret;

error:
	iris_helper_buffers_done(inst, q->type, VB2_BUF_STATE_QUEUED);
	iris_inst_change_state(inst, IRIS_INST_ERROR);
	mutex_unlock(&inst->lock);

	return ret;
}

void iris_vb2_stop_streaming(struct vb2_queue *q)
{
	struct iris_inst *inst;
	int ret = 0;

	inst = vb2_get_drv_priv(q);

	if (V4L2_TYPE_IS_CAPTURE(q->type) && inst->state == IRIS_INST_INIT) {
		iris_release_core_load(inst);
		return;
	}

	mutex_lock(&inst->lock);
	if (inst->domain == DECODER && V4L2_TYPE_IS_CAPTURE(q->type))
		iris_vdec_clear_pending_output(inst);

	if (!V4L2_TYPE_IS_OUTPUT(q->type) &&
	    !V4L2_TYPE_IS_CAPTURE(q->type))
		goto exit;

	ret = iris_session_streamoff(inst, q->type);
	if (ret)
		goto exit;
	if (V4L2_TYPE_IS_CAPTURE(q->type))
		iris_release_core_load(inst);

exit:
	iris_helper_buffers_done(inst, q->type, VB2_BUF_STATE_ERROR);
	if (ret)
		iris_inst_change_state(inst, IRIS_INST_ERROR);

	mutex_unlock(&inst->lock);
}

int iris_vb2_buf_prepare(struct vb2_buffer *vb)
{
	struct iris_inst *inst = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	if (V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type)) {
		if (vbuf->field == V4L2_FIELD_ANY)
			vbuf->field = V4L2_FIELD_NONE;
		if (vbuf->field != V4L2_FIELD_NONE)
			return -EINVAL;
	}

	if (!(inst->sub_state & IRIS_INST_SUB_DRC)) {
		if (vb->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
		    vb2_plane_size(vb, 0) < iris_get_buffer_size(inst, BUF_OUTPUT))
			return -EINVAL;
		if (vb->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE &&
		    vb2_plane_size(vb, 0) < iris_get_buffer_size(inst, BUF_INPUT))
			return -EINVAL;
	}
	return 0;
}

int iris_vb2_buf_out_validate(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *v4l2_buf = to_vb2_v4l2_buffer(vb);

	v4l2_buf->field = V4L2_FIELD_NONE;

	return 0;
}

void iris_vb2_buf_queue(struct vb2_buffer *vb2)
{
	static const struct v4l2_event eos = { .type = V4L2_EVENT_EOS };
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb2);
	struct v4l2_m2m_ctx *m2m_ctx;
	struct iris_inst *inst;
	int ret = 0;

	inst = vb2_get_drv_priv(vb2->vb2_queue);

	mutex_lock(&inst->lock);
	if (inst->state == IRIS_INST_ERROR) {
		ret = -EBUSY;
		goto exit;
	}

	if (vbuf->field == V4L2_FIELD_ANY)
		vbuf->field = V4L2_FIELD_NONE;

	m2m_ctx = inst->m2m_ctx;

	if (!vb2->planes[0].bytesused && V4L2_TYPE_IS_OUTPUT(vb2->type)) {
		ret = -EINVAL;
		goto exit;
	}

	if (!inst->last_buffer_dequeued && V4L2_TYPE_IS_CAPTURE(vb2->vb2_queue->type)) {
		if ((inst->sub_state & IRIS_INST_SUB_DRC &&
		     inst->sub_state & IRIS_INST_SUB_DRC_LAST) ||
		    (inst->sub_state & IRIS_INST_SUB_DRAIN &&
		     inst->sub_state & IRIS_INST_SUB_DRAIN_LAST)) {
			vbuf->flags |= V4L2_BUF_FLAG_LAST;
			vbuf->sequence = inst->sequence_cap++;
			vbuf->field = V4L2_FIELD_NONE;
			vb2_set_plane_payload(vb2, 0, 0);
			v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_DONE);
			if (!v4l2_m2m_has_stopped(m2m_ctx)) {
				v4l2_event_queue_fh(&inst->fh, &eos);
				v4l2_m2m_mark_stopped(m2m_ctx);
			}
			inst->last_buffer_dequeued = true;
			goto exit;
		}
	}

	v4l2_m2m_buf_queue(m2m_ctx, vbuf);

	if (inst->domain == DECODER)
		ret = iris_vdec_qbuf(inst, vbuf);
	else
		ret = iris_venc_qbuf(inst, vbuf);

exit:
	if (ret) {
		dev_err_ratelimited(inst->core->dev, "QBUF failed: type=%u index=%u ret=%d\n",
				   vb2->type, vb2->index, ret);
		iris_inst_change_state(inst, IRIS_INST_ERROR);
		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}
	mutex_unlock(&inst->lock);
}
