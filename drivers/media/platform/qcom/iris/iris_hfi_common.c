// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/irq.h>
#include <linux/pm_runtime.h>

#include "iris_firmware.h"
#include "iris_core.h"
#include "iris_hfi_common.h"
#include "iris_vpu_common.h"

u32 iris_hfi_get_v4l2_color_primaries(u32 hfi_primaries)
{
	switch (hfi_primaries) {
	case HFI_PRIMARIES_RESERVED:
		return V4L2_COLORSPACE_DEFAULT;
	case HFI_PRIMARIES_BT709:
		return V4L2_COLORSPACE_REC709;
	case HFI_PRIMARIES_BT470_SYSTEM_M:
		return V4L2_COLORSPACE_470_SYSTEM_M;
	case HFI_PRIMARIES_BT470_SYSTEM_BG:
		return V4L2_COLORSPACE_470_SYSTEM_BG;
	case HFI_PRIMARIES_BT601_525:
		return V4L2_COLORSPACE_SMPTE170M;
	case HFI_PRIMARIES_SMPTE_ST240M:
		return V4L2_COLORSPACE_SMPTE240M;
	case HFI_PRIMARIES_BT2020:
		return V4L2_COLORSPACE_BT2020;
	case V4L2_COLORSPACE_DCI_P3:
		return HFI_PRIMARIES_SMPTE_RP431_2;
	default:
		return V4L2_COLORSPACE_DEFAULT;
	}
}

u32 iris_hfi_get_v4l2_transfer_char(u32 hfi_characterstics)
{
	switch (hfi_characterstics) {
	case HFI_TRANSFER_RESERVED:
		return V4L2_XFER_FUNC_DEFAULT;
	case HFI_TRANSFER_BT709:
		return V4L2_XFER_FUNC_709;
	case HFI_TRANSFER_SMPTE_ST240M:
		return V4L2_XFER_FUNC_SMPTE240M;
	case HFI_TRANSFER_SRGB_SYCC:
		return V4L2_XFER_FUNC_SRGB;
	case HFI_TRANSFER_SMPTE_ST2084_PQ:
		return V4L2_XFER_FUNC_SMPTE2084;
	default:
		return V4L2_XFER_FUNC_DEFAULT;
	}
}

u32 iris_hfi_get_v4l2_matrix_coefficients(u32 hfi_coefficients)
{
	switch (hfi_coefficients) {
	case HFI_MATRIX_COEFF_RESERVED:
		return V4L2_YCBCR_ENC_DEFAULT;
	case HFI_MATRIX_COEFF_BT709:
		return V4L2_YCBCR_ENC_709;
	case HFI_MATRIX_COEFF_BT470_SYS_BG_OR_BT601_625:
		return V4L2_YCBCR_ENC_XV601;
	case HFI_MATRIX_COEFF_BT601_525_BT1358_525_OR_625:
		return V4L2_YCBCR_ENC_601;
	case HFI_MATRIX_COEFF_SMPTE_ST240:
		return V4L2_YCBCR_ENC_SMPTE240M;
	case HFI_MATRIX_COEFF_BT2020_NON_CONSTANT:
		return V4L2_YCBCR_ENC_BT2020;
	case HFI_MATRIX_COEFF_BT2020_CONSTANT:
		return V4L2_YCBCR_ENC_BT2020_CONST_LUM;
	default:
		return V4L2_YCBCR_ENC_DEFAULT;
	}
}

int iris_hfi_core_init(struct iris_core *core)
{
	const struct iris_hfi_command_ops *hfi_ops = core->hfi_ops;
	int ret;

	/*
	 * Wait for the VPU5 SYS_INIT response before sending any further
	 * command.  The downstream Venus driver sends its default system
	 * properties later, immediately before SESSION_INIT.
	 */
	if (core->iris_platform_data->legacy_vpu5) {
		dev_info(core->dev,
			 "Iris1 v88: sending SYS_INIT through legacy HFI queue\n");
		return hfi_ops->sys_init(core);
	}

	ret = hfi_ops->sys_init(core);
	if (ret)
		return ret;

	ret = hfi_ops->sys_image_version(core);
	if (ret)
		return ret;

	return hfi_ops->sys_interframe_powercollapse(core);
}

irqreturn_t iris_hfi_isr(int irq, void *data)
{
	struct iris_core *core = data;

	disable_irq_nosync(irq);

	/*
	 * SM8150 VPU5 signals a level-high interrupt.  Clear the source in
	 * the hard IRQ handler, as the legacy Venus driver does.  Deferring
	 * the clear leaves the source asserted during firmware boot because
	 * iris_core_init() owns core->lock while the threaded handler waits.
	 */
	if (core && core->iris_platform_data->legacy_vpu5)
		iris_vpu_clear_interrupt(core);

	return IRQ_WAKE_THREAD;
}

irqreturn_t iris_hfi_isr_handler(int irq, void *data)
{
	struct iris_core *core = data;

	if (!core)
		return IRQ_NONE;

	mutex_lock(&core->lock);
	pm_runtime_mark_last_busy(core->dev);
	if (!core->iris_platform_data->legacy_vpu5)
		iris_vpu_clear_interrupt(core);
	mutex_unlock(&core->lock);

	core->hfi_response_ops->hfi_response_handler(core);

	if (!iris_vpu_watchdog(core, core->intr_status))
		enable_irq(irq);

	return IRQ_HANDLED;
}

int iris_hfi_pm_suspend(struct iris_core *core)
{
	int ret;

	/*
	 * SM8150 VPU5 has no supported power-collapse sequence.  Keep the
	 * controller powered and report success so system suspend is not
	 * vetoed (iris_vpu_prepare_pc() would return -EAGAIN).
	 */
	if (core->iris_platform_data->legacy_vpu5)
		return 0;

	ret = iris_vpu_prepare_pc(core);
	if (ret) {
		pm_runtime_mark_last_busy(core->dev);
		ret = -EAGAIN;
		goto error;
	}

	ret = iris_set_hw_state(core, false);
	if (ret)
		goto error;

	iris_vpu_power_off(core);

	return 0;

error:
	dev_err_once(core->dev,
		     "failed to suspend (%d); suppressing repeated messages\n", ret);

	return ret;
}

void iris_wake_restart_worker(struct work_struct *work)
{
	struct iris_core *core =
		container_of(work, struct iris_core, wake_restart.work);
	int ret;

	if (core->irq >= 0 &&
	    irqd_irq_disabled(irq_get_irq_data(core->irq)))
		enable_irq(core->irq);

	iris_core_deinit(core);
	ret = iris_core_init(core);
	if (ret)
		dev_err(core->dev,
			"VPU firmware restart failed after s2idle resume: %d\n"
			"(firmware may still be wedged; reboot clears)\n",
			ret);
	else
		dev_info(core->dev,
			 "restarted VPU firmware after s2idle resume\n");
}

int iris_hfi_pm_resume(struct iris_core *core)
{
	const struct iris_hfi_command_ops *ops = core->hfi_ops;
	int ret;

	/*
	 * VPU5 stays powered through s2idle, but the HFI transport wedges at
	 * wake: SESSION_INIT responses stop arriving until the firmware is
	 * re-initialized (full teardown), as a SYS_INIT re-send is insufficient.
	 */
	if (core->iris_platform_data->legacy_vpu5) {
		/*
		 * A synchronous rebuild here would deadlock: running inside
		 * dpm_resume, the VPU clock and power domains are still
		 * resuming.  Queue the restart in process context instead,
		 * which replays firmware-error recovery (deinit + init) after
		 * resume and clears the s2idle wedge before any session opens.
		 */
		schedule_delayed_work(&core->wake_restart,
				      msecs_to_jiffies(1000));
		return 0;
	}

	ret = iris_vpu_power_on(core);
	if (ret)
		goto error;

	ret = iris_set_hw_state(core, true);
	if (ret)
		goto err_power_off;

	ret = iris_vpu_boot_firmware(core);
	if (ret)
		goto err_suspend_hw;

	ret = ops->sys_interframe_powercollapse(core);
	if (ret)
		goto err_suspend_hw;

	return 0;

err_suspend_hw:
	iris_set_hw_state(core, false);
err_power_off:
	iris_vpu_power_off(core);
error:
	dev_err(core->dev, "failed to resume\n");

	return -EBUSY;
}
