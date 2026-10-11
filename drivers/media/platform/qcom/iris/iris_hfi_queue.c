// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/delay.h>
#include <linux/iommu.h>
#include <linux/pm_runtime.h>

#include "iris_core.h"
#include "iris_hfi_queue.h"
#include "iris_vpu_common.h"

static u32 iris_hfi_queue_table_size(struct iris_core *core)
{
	if (core->iris_platform_data->legacy_vpu5)
		return sizeof(struct iris_hfi_legacy_queue_table_header);

	return sizeof(struct iris_hfi_queue_table_header);
}

static u32 iris_hfi_queue_alloc_size(struct iris_core *core)
{
	u32 queue_size;

	queue_size = ALIGN(iris_hfi_queue_table_size(core) +
			   (IFACEQ_QUEUE_SIZE * IFACEQ_NUMQ), SZ_4K);

	/*
	 * VPU5 firmware is given a 1 MiB-aligned UC region size.  Unlike the
	 * downstream allocator, dma_alloc_attrs() does not guarantee that the
	 * separately allocated SFR and the unused tail are adjacent to the
	 * queue IOVA.  Map the complete advertised region so speculative or
	 * bounds-checking firmware accesses cannot fault in its unmapped tail.
	 */
	if (core->iris_platform_data->legacy_vpu5)
		return ALIGN(SFR_SIZE + queue_size, SZ_1M);

	return queue_size;
}

static int iris_hfi_queue_write(struct iris_iface_q_info *qinfo, void *packet, u32 packet_size)
{
	struct iris_hfi_queue_header *queue = qinfo->qhdr;
	u32 write_idx = queue->write_idx * sizeof(u32);
	u32 read_idx = queue->read_idx * sizeof(u32);
	u32 empty_space, new_write_idx, residue;
	u32 *write_ptr;

	if (write_idx < read_idx)
		empty_space = read_idx - write_idx;
	else
		empty_space = IFACEQ_QUEUE_SIZE - (write_idx -  read_idx);
	if (empty_space < packet_size)
		return -ENOSPC;

	queue->tx_req =  0;

	new_write_idx = write_idx + packet_size;
	write_ptr = (u32 *)((u8 *)qinfo->kernel_vaddr + write_idx);

	if (write_ptr < (u32 *)qinfo->kernel_vaddr ||
	    write_ptr > (u32 *)(qinfo->kernel_vaddr +
	    IFACEQ_QUEUE_SIZE))
		return -EINVAL;

	if (new_write_idx < IFACEQ_QUEUE_SIZE) {
		memcpy(write_ptr, packet, packet_size);
	} else {
		residue = new_write_idx - IFACEQ_QUEUE_SIZE;
		memcpy(write_ptr, packet, (packet_size - residue));
		memcpy(qinfo->kernel_vaddr,
		       packet + (packet_size - residue), residue);
		new_write_idx = residue;
	}

	/* Make sure packet is written before updating the write index */
	mb();
	queue->write_idx = new_write_idx / sizeof(u32);

	/* Make sure write index is updated before an interrupt is raised */
	mb();

	return 0;
}

static int iris_hfi_queue_read(struct iris_iface_q_info *qinfo, void *packet)
{
	struct iris_hfi_queue_header *queue = qinfo->qhdr;
	u32 write_idx = queue->write_idx * sizeof(u32);
	u32 read_idx = queue->read_idx * sizeof(u32);
	u32 packet_size, receive_request = 0;
	u32 new_read_idx, residue;
	u32 *read_ptr;
	int ret = 0;

	if (queue->queue_type == IFACEQ_MSGQ_ID)
		receive_request = 1;

	if (read_idx == write_idx) {
		queue->rx_req = receive_request;
		/* Ensure qhdr is updated in main memory */
		mb();
		return -ENODATA;
	}

	read_ptr = qinfo->kernel_vaddr + read_idx;
	if (read_ptr < (u32 *)qinfo->kernel_vaddr ||
	    read_ptr > (u32 *)(qinfo->kernel_vaddr +
	    IFACEQ_QUEUE_SIZE - sizeof(*read_ptr)))
		return -ENODATA;

	packet_size = *read_ptr;
	if (!packet_size)
		return -EINVAL;

	new_read_idx = read_idx + packet_size;
	if (packet_size <= IFACEQ_CORE_PKT_SIZE) {
		if (new_read_idx < IFACEQ_QUEUE_SIZE) {
			memcpy(packet, read_ptr, packet_size);
		} else {
			residue = new_read_idx - IFACEQ_QUEUE_SIZE;
			memcpy(packet, read_ptr, (packet_size - residue));
			memcpy((packet + (packet_size - residue)),
			       qinfo->kernel_vaddr, residue);
			new_read_idx = residue;
		}
	} else {
		new_read_idx = write_idx;
		ret = -EBADMSG;
	}

	queue->rx_req = receive_request;

	queue->read_idx = new_read_idx / sizeof(u32);
	/* Ensure qhdr is updated in main memory */
	mb();

	return ret;
}

int iris_hfi_queue_cmd_write_locked(struct iris_core *core, void *pkt, u32 pkt_size)
{
	struct iris_iface_q_info *q_info = &core->command_queue;
	struct iris_hfi_queue_header *qhdr = q_info->qhdr;
	u32 *words = pkt;

	if (core->state == IRIS_CORE_ERROR || core->state == IRIS_CORE_DEINIT)
		return -EINVAL;

	if (!iris_hfi_queue_write(q_info, pkt, pkt_size)) {
		dev_dbg(core->dev,
			 "HFI cmd: bytes=%u words=%#x,%#x,%#x,%#x,%#x; q status=%#x start=%#x type=%#x size=%u rx_req=%u tx_req=%u r=%u w=%u\n",
			 pkt_size, words[0], words[1],
			 pkt_size >= 3 * sizeof(*words) ? words[2] : 0,
			 pkt_size >= 4 * sizeof(*words) ? words[3] : 0,
			 pkt_size >= 5 * sizeof(*words) ? words[4] : 0,
			 qhdr->status, qhdr->start_addr,
			 ((u32)qhdr->header_type << 16) | qhdr->queue_type,
			 qhdr->q_size, qhdr->rx_req, qhdr->tx_req,
			 qhdr->read_idx, qhdr->write_idx);
		iris_vpu_raise_interrupt(core);
	} else {
		dev_err(core->dev, "queue full\n");
		return -ENODATA;
	}

	return 0;
}

int iris_hfi_queue_cmd_write(struct iris_core *core, void *pkt, u32 pkt_size)
{
	int ret;

	ret = pm_runtime_resume_and_get(core->dev);
	if (ret < 0)
		goto exit;

	mutex_lock(&core->lock);
	ret = iris_hfi_queue_cmd_write_locked(core, pkt, pkt_size);
	if (ret) {
		mutex_unlock(&core->lock);
		goto exit;
	}
	mutex_unlock(&core->lock);

	pm_runtime_put_autosuspend(core->dev);

	return 0;

exit:
	pm_runtime_put_sync(core->dev);

	return ret;
}

int iris_hfi_queue_msg_read(struct iris_core *core, void *pkt)
{
	struct iris_iface_q_info *q_info = &core->message_queue;
	int ret = 0;

	mutex_lock(&core->lock);
	if (core->state != IRIS_CORE_INIT) {
		ret = -EINVAL;
		goto unlock;
	}

	if (iris_hfi_queue_read(q_info, pkt)) {
		ret = -ENODATA;
		goto unlock;
	}

unlock:
	mutex_unlock(&core->lock);

	return ret;
}

int iris_hfi_queue_dbg_read(struct iris_core *core, void *pkt)
{
	struct iris_iface_q_info *q_info = &core->debug_queue;
	int ret = 0;

	mutex_lock(&core->lock);
	if (core->state != IRIS_CORE_INIT) {
		ret = -EINVAL;
		goto unlock;
	}

	if (iris_hfi_queue_read(q_info, pkt)) {
		ret = -ENODATA;
		goto unlock;
	}

unlock:
	mutex_unlock(&core->lock);

	return ret;
}

static void iris_hfi_queue_set_header(struct iris_core *core, u32 queue_id,
				      struct iris_iface_q_info *iface_q)
{
	iface_q->qhdr->status = 0x1;
	iface_q->qhdr->start_addr = iface_q->device_addr;
	iface_q->qhdr->header_type = IFACEQ_DFLT_QHDR;
	iface_q->qhdr->queue_type = queue_id;
	iface_q->qhdr->q_size = IFACEQ_QUEUE_SIZE / sizeof(u32);
	iface_q->qhdr->pkt_size = 0; /* variable packet size */
	iface_q->qhdr->rx_wm = 0x1;
	iface_q->qhdr->tx_wm = 0x1;
	iface_q->qhdr->rx_req = 0x1;
	iface_q->qhdr->tx_req = 0x0;
	iface_q->qhdr->rx_irq_status = 0x0;
	iface_q->qhdr->tx_irq_status = 0x0;
	iface_q->qhdr->read_idx = 0x0;
	iface_q->qhdr->write_idx = 0x0;

	/*
	 * Set receive request to zero on debug queue as there is no
	 * need of interrupt from video hardware for debug messages
	 */
	if (queue_id == IFACEQ_DBGQ_ID)
		iface_q->qhdr->rx_req = 0;
}

static void
iris_hfi_queue_init(struct iris_core *core, u32 queue_id, struct iris_iface_q_info *iface_q)
{
	struct iris_hfi_queue_table_header *q_tbl_hdr = core->iface_q_table_vaddr;
	struct iris_hfi_legacy_queue_table_header *legacy_tbl_hdr =
		core->iface_q_table_vaddr;
	u32 offset = iris_hfi_queue_table_size(core) +
		     (queue_id * IFACEQ_QUEUE_SIZE);

	iface_q->device_addr = core->iface_q_table_daddr + offset;
	iface_q->kernel_vaddr =
			(void *)((char *)core->iface_q_table_vaddr + offset);
	if (core->iris_platform_data->legacy_vpu5)
		iface_q->qhdr = &legacy_tbl_hdr->q_hdr[queue_id];
	else
		iface_q->qhdr = &q_tbl_hdr->q_hdr[queue_id];

	iris_hfi_queue_set_header(core, queue_id, iface_q);
}

static void iris_hfi_queue_deinit(struct iris_iface_q_info *iface_q)
{
	iface_q->qhdr = NULL;
	iface_q->kernel_vaddr = NULL;
	iface_q->device_addr = 0;
}

int iris_hfi_queues_init(struct iris_core *core)
{
	struct iris_hfi_queue_table_header *q_tbl_hdr;
	struct iris_hfi_legacy_queue_table_header *legacy_tbl_hdr;
	struct iommu_domain *domain;
	phys_addr_t first_phys, last_phys;
	u32 queue_size, queue_used;

	/* Iris hardware requires 4K queue alignment */
	queue_used = ALIGN(iris_hfi_queue_table_size(core) +
			   (IFACEQ_QUEUE_SIZE * IFACEQ_NUMQ), SZ_4K);
	queue_size = iris_hfi_queue_alloc_size(core);
	dev_info(core->dev,
		 "HFI queues: allocating %u-byte queue region (%u bytes used)\n",
		 queue_size, queue_used);
	core->iface_q_table_vaddr = dma_alloc_attrs(core->dev, queue_size,
						    &core->iface_q_table_daddr,
						    GFP_KERNEL, DMA_ATTR_WRITE_COMBINE);
	if (!core->iface_q_table_vaddr) {
		dev_err(core->dev, "queues alloc and map failed\n");
		return -ENOMEM;
	}
	memset(core->iface_q_table_vaddr, 0, queue_size);
	dev_info(core->dev, "HFI queues: queue table allocated at %pad\n",
		 &core->iface_q_table_daddr);

	if (core->iris_platform_data->legacy_vpu5) {
		domain = iommu_get_domain_for_dev(core->dev);
		if (!domain) {
			dev_warn(core->dev,
				 "HFI queues: no IOMMU domain attached to video device\n");
		} else {
			first_phys = iommu_iova_to_phys(domain,
						       core->iface_q_table_daddr);
			last_phys = iommu_iova_to_phys(domain,
						      core->iface_q_table_daddr +
						      queue_size - 1);
			dev_info(core->dev,
				 "HFI queues: IOMMU coverage IOVA %pad..%pad -> phys %pa..%pa\n",
				 &core->iface_q_table_daddr,
				 &(dma_addr_t) {
					core->iface_q_table_daddr + queue_size - 1
				 },
				 &first_phys, &last_phys);
			if (!first_phys || !last_phys)
				dev_err(core->dev,
					"HFI queues: queue IOVA is not fully mapped\n");
		}
	}

	dev_info(core->dev, "HFI queues: allocating %u-byte SFR buffer\n",
		 SFR_SIZE);
	core->sfr_vaddr = dma_alloc_attrs(core->dev, SFR_SIZE,
					  &core->sfr_daddr,
					  GFP_KERNEL, DMA_ATTR_WRITE_COMBINE);
	if (!core->sfr_vaddr) {
		dev_err(core->dev, "sfr alloc and map failed\n");
		dma_free_attrs(core->dev, queue_size, core->iface_q_table_vaddr,
			       core->iface_q_table_daddr, DMA_ATTR_WRITE_COMBINE);
		return -ENOMEM;
	}
	dev_info(core->dev, "HFI queues: SFR buffer allocated at %pad\n",
		 &core->sfr_daddr);

	dev_info(core->dev, "HFI queues: initializing command queue\n");
	iris_hfi_queue_init(core, IFACEQ_CMDQ_ID, &core->command_queue);
	dev_info(core->dev, "HFI queues: initializing message queue\n");
	iris_hfi_queue_init(core, IFACEQ_MSGQ_ID, &core->message_queue);
	dev_info(core->dev, "HFI queues: initializing debug queue\n");
	iris_hfi_queue_init(core, IFACEQ_DBGQ_ID, &core->debug_queue);

	dev_info(core->dev, "HFI queues: initializing table header\n");
	if (core->iris_platform_data->legacy_vpu5) {
		legacy_tbl_hdr = core->iface_q_table_vaddr;
		legacy_tbl_hdr->version = 0;
		legacy_tbl_hdr->size = sizeof(*legacy_tbl_hdr);
		legacy_tbl_hdr->qhdr0_offset =
			offsetof(struct iris_hfi_legacy_queue_table_header, q_hdr);
		legacy_tbl_hdr->qhdr_size =
			sizeof(legacy_tbl_hdr->q_hdr[0]);
		legacy_tbl_hdr->num_q = IFACEQ_NUMQ;
		legacy_tbl_hdr->num_active_q = IFACEQ_NUMQ;
		dev_info(core->dev,
			 "HFI queues: legacy Venus table size %zu, first header offset %u\n",
			 sizeof(*legacy_tbl_hdr),
			 legacy_tbl_hdr->qhdr0_offset);
	} else {
		q_tbl_hdr = core->iface_q_table_vaddr;
		q_tbl_hdr->version = 0;
		q_tbl_hdr->device_addr = (void *)core;
		strscpy(q_tbl_hdr->name, "msm_v4l2_vidc",
			sizeof(q_tbl_hdr->name));
		q_tbl_hdr->size = sizeof(*q_tbl_hdr);
		q_tbl_hdr->qhdr0_offset = sizeof(*q_tbl_hdr) -
			(IFACEQ_NUMQ * sizeof(struct iris_hfi_queue_header));
		q_tbl_hdr->qhdr_size = sizeof(q_tbl_hdr->q_hdr[0]);
		q_tbl_hdr->num_q = IFACEQ_NUMQ;
		q_tbl_hdr->num_active_q = IFACEQ_NUMQ;
	}

	 /* Write sfr size in first word to be used by firmware */
	*((u32 *)core->sfr_vaddr) = SFR_SIZE;
	/*
	 * Publish the complete legacy table, all queue headers and the SFR
	 * header before the firmware is allowed to fetch them.  The Venus
	 * HFI Gen1 implementation has the same final barrier.
	 */
	wmb();
	dev_info(core->dev, "HFI queues: initialization complete\n");

	return 0;
}

void iris_hfi_queues_deinit(struct iris_core *core)
{
	u32 queue_size;

	if (!core->iface_q_table_vaddr)
		return;

	iris_hfi_queue_deinit(&core->debug_queue);
	iris_hfi_queue_deinit(&core->message_queue);
	iris_hfi_queue_deinit(&core->command_queue);

	dma_free_attrs(core->dev, SFR_SIZE, core->sfr_vaddr,
		       core->sfr_daddr, DMA_ATTR_WRITE_COMBINE);

	core->sfr_vaddr = NULL;
	core->sfr_daddr = 0;

	queue_size = iris_hfi_queue_alloc_size(core);

	dma_free_attrs(core->dev, queue_size, core->iface_q_table_vaddr,
		       core->iface_q_table_daddr, DMA_ATTR_WRITE_COMBINE);

	core->iface_q_table_vaddr = NULL;
	core->iface_q_table_daddr = 0;
}
