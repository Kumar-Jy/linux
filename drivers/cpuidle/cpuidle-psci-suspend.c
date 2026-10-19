// SPDX-License-Identifier: GPL-2.0-only
/*
 * PSCI-based PM_SUSPEND_MEM backend for platforms whose firmware does not
 * implement PSCI SYSTEM_SUSPEND (most upstream Qualcomm SoCs: sm8150,
 * sdm845, sm8250, sc7180, sc8280xp, ...).
 *
 * The mechanism reuses the OSI hierarchical cpuidle path's syscore hook.
 * After kernel/power/suspend.c has hot-unplugged the secondary CPUs and run
 * syscore_suspend(), psci_idle_syscore_suspend() (drivers/cpuidle/cpuidle-psci.c)
 * drives every CPU's per-PD device to suspend via dev_pm_genpd_suspend();
 * when the last one is suspended the cluster genpd transitions to OFF,
 * which fires psci_pd_power_off() (drivers/cpuidle/cpuidle-psci-domain.c)
 * and stamps the cluster's PSCI suspend-param (e.g. 0x4100c244 on sm8150)
 * into the boot CPU's psci_domain_state slot.  As a side effect, the
 * GENPD_NOTIFY_PRE_OFF notifier chain calls rpmh_rsc_pd_callback()
 * (drivers/soc/qcom/rpmh-rsc.c) which flushes RPMh sleep/wake votes to the
 * controller.
 *
 * All this happens before suspend_ops->enter() is called.  Our .enter()
 * therefore just reads the stamped state back and issues the firmware
 * CPU_SUSPEND request -- the same one __psci_enter_domain_idle_state()
 * makes for the deepest cluster state, just driven from the system-suspend
 * path instead of the cpuidle path.
 *
 * If firmware later starts advertising PSCI_FN_NATIVE(1_0, SYSTEM_SUSPEND),
 * drivers/firmware/psci/psci.c registers its own psci_suspend_ops at
 * subsys_initcall and we detect that via suspend_ops_is_set() at our
 * late_initcall, gracefully becoming a no-op.
 */

#define pr_fmt(fmt) "psci-suspend: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/psci.h>
#include <linux/suspend.h>

#include "cpuidle-psci.h"

static int psci_system_suspend_enter(suspend_state_t state)
{
	u32 param;
	int ret;

	if (WARN_ON_ONCE(state != PM_SUSPEND_MEM))
		return -EINVAL;

	/*
	 * Stamped by psci_pd_power_off() during syscore_suspend.  IRQs are
	 * off and only the boot CPU is online at this point, so the per-CPU
	 * access is stable.
	 */
	param = psci_idle_suspend_param();
	if (!param) {
		pr_err("no cluster suspend state recorded; cluster genpd not collapsed?\n");
		return -EINVAL;
	}

	/*
	 * psci_cpu_suspend_enter() takes the cpu_suspend(state,
	 * psci_suspend_finisher) slow path for context-losing states, which
	 * is what cluster collapse needs.
	 */
	ret = psci_cpu_suspend_enter(param);
	if (ret) {
		pr_err("psci_cpu_suspend_enter(%#x) failed: %d\n", param, ret);
		return -EIO;
	}

	return 0;
}

static const struct platform_suspend_ops psci_system_suspend_ops = {
	.valid	= suspend_valid_only_mem,
	.enter	= psci_system_suspend_enter,
};

static int __init psci_idle_init_suspend(void)
{
	int ret;

	/*
	 * If drivers/firmware/psci/psci.c already registered the canonical
	 * SYSTEM_SUSPEND backend at subsys_initcall, stay out of its way.
	 */
	if (suspend_ops_is_set())
		return 0;

	/*
	 * We rely on cpuidle-psci's syscore op to collapse the cluster genpd
	 * before our .enter runs.  Without OSI + hierarchical genpd that
	 * never happens and psci_idle_suspend_param() would always be 0.
	 */
	if (!psci_cpuidle_uses_syscore())
		return 0;

	if (!psci_ops.cpu_suspend) {
		pr_warn("PSCI CPU_SUSPEND not available\n");
		return -ENODEV;
	}

	ret = suspend_set_ops_if_unused(&psci_system_suspend_ops);
	if (ret == -EBUSY)
		return 0; /* Lost the race; nothing to do. */
	if (ret) {
		pr_err("failed to install suspend ops: %d\n", ret);
		return ret;
	}

	pr_info("registered PM_SUSPEND_MEM backend (PSCI OSI cluster)\n");
	return 0;
}
late_initcall(psci_idle_init_suspend);
