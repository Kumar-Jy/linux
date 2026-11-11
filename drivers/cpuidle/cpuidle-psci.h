/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __CPUIDLE_PSCI_H
#define __CPUIDLE_PSCI_H

struct device_node;
struct generic_pm_domain;

void psci_set_domain_state(struct generic_pm_domain *pd, unsigned int state_idx,
			   u32 state);
int psci_dt_parse_state_node(struct device_node *np, u32 *state);

/*
 * Snapshot of the most recent cluster idle state voted into psci_domain_state
 * on the current CPU.  Used by the PM_SUSPEND_MEM backend in
 * drivers/cpuidle/cpuidle-psci-suspend.c after syscore_suspend has driven the
 * cluster genpd off (which stamps the cluster-sleep PSCI state into the boot
 * CPU's psci_domain_state slot).  Returns 0 if nothing was stamped.
 */
u32 psci_idle_suspend_param(void);

/*
 * Reports whether the syscore + hierarchical-genpd path is in use, i.e. OSI
 * mode and at least one cluster domain idle state was parsed from DT.  When
 * this is false, psci_idle_suspend_param() will never see a non-zero value
 * and the PM_SUSPEND_MEM backend should refuse to register.
 */
bool psci_cpuidle_uses_syscore(void);

#endif /* __CPUIDLE_PSCI_H */
