// SPDX-License-Identifier: GPL-2.0-only
/*
 * kernel/power/suspend.c - Suspend to RAM and standby functionality.
 *
 * Copyright (c) 2003 Patrick Mochel
 * Copyright (c) 2003 Open Source Development Lab
 * Copyright (c) 2009 Rafael J. Wysocki <rjw@sisk.pl>, Novell Inc.
 */

#define pr_fmt(fmt) "PM: " fmt

#include <linux/string.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/cpu.h>
#include <linux/cpuidle.h>
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/timekeeping.h>
#include <clocksource/arm_arch_timer.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>
#include <linux/swait.h>
#include <linux/ftrace.h>
#include <trace/events/power.h>
#include <linux/compiler.h>
#include <linux/moduleparam.h>
#include <linux/fs.h>

#include "power.h"

const char * const pm_labels[] = {
	[PM_SUSPEND_TO_IDLE] = "freeze",
	[PM_SUSPEND_STANDBY] = "standby",
	[PM_SUSPEND_MEM] = "mem",
};
const char *pm_states[PM_SUSPEND_MAX];
static const char * const mem_sleep_labels[] = {
	[PM_SUSPEND_TO_IDLE] = "s2idle",
	[PM_SUSPEND_STANDBY] = "shallow",
	[PM_SUSPEND_MEM] = "deep",
};
const char *mem_sleep_states[PM_SUSPEND_MAX];

suspend_state_t mem_sleep_current = PM_SUSPEND_TO_IDLE;
suspend_state_t mem_sleep_default = PM_SUSPEND_MAX;
suspend_state_t pm_suspend_target_state;
EXPORT_SYMBOL_GPL(pm_suspend_target_state);

unsigned int pm_suspend_global_flags;
EXPORT_SYMBOL_GPL(pm_suspend_global_flags);

static const struct platform_suspend_ops *suspend_ops;
static const struct platform_s2idle_ops *s2idle_ops;
static DECLARE_SWAIT_QUEUE_HEAD(s2idle_wait_head);

enum s2idle_states __read_mostly s2idle_state;
static DEFINE_RAW_SPINLOCK(s2idle_lock);

/**
 * pm_suspend_default_s2idle - Check if suspend-to-idle is the default suspend.
 *
 * Return 'true' if suspend-to-idle has been selected as the default system
 * suspend method.
 */
bool pm_suspend_default_s2idle(void)
{
	return mem_sleep_current == PM_SUSPEND_TO_IDLE;
}
EXPORT_SYMBOL_GPL(pm_suspend_default_s2idle);

void s2idle_set_ops(const struct platform_s2idle_ops *ops)
{
	unsigned int sleep_flags;

	sleep_flags = lock_system_sleep();
	s2idle_ops = ops;
	unlock_system_sleep(sleep_flags);
}

#define PM_S2IDLE_GRACE_MSEC		300

static u64 s2idle_grace_end_ns;
static bool s2idle_grace_active;
static int s2idle_grace_wake_irq = -1;

/*
 * Time source for the grace window.  ktime_get_ns() is frozen by
 * timekeeping_suspend() for the whole s2idle sleep and re-inserted at the
 * frozen value on resume, so a wall-clock grace can never expire while the
 * system is asleep and would swallow real wake events (e.g. the power key).
 * The ARM system counter keeps counting while timekeeping is frozen, so use
 * it to measure real elapsed time.  The ktime fallback is only for platforms
 * without the arch timer.
 */
static u64 s2idle_grace_now_ns(void)
{
	if (arch_timer_read_counter && arch_timer_get_rate())
		return mul_u64_u32_div(arch_timer_read_counter(),
				       NSEC_PER_SEC, arch_timer_get_rate());

	return ktime_get_ns();
}

void pm_s2idle_grace_start(void)
{
	if (READ_ONCE(pm_suspend_target_state) != PM_SUSPEND_TO_IDLE ||
	    READ_ONCE(s2idle_grace_wake_irq) < 0)
		return;

	WRITE_ONCE(s2idle_grace_end_ns,
		   s2idle_grace_now_ns() + (PM_S2IDLE_GRACE_MSEC * NSEC_PER_MSEC));
	WRITE_ONCE(s2idle_grace_active, true);
}
EXPORT_SYMBOL_GPL(pm_s2idle_grace_start);

void pm_s2idle_grace_end(void)
{
	WRITE_ONCE(s2idle_grace_active, false);
}
EXPORT_SYMBOL_GPL(pm_s2idle_grace_end);

void pm_s2idle_set_wake_irq(int irq)
{
	WRITE_ONCE(s2idle_grace_wake_irq, irq);
}
EXPORT_SYMBOL_GPL(pm_s2idle_set_wake_irq);

bool pm_s2idle_grace_ignore_wakeup_irq(unsigned int irq)
{
	if (!READ_ONCE(s2idle_grace_active))
		return false;

	if (READ_ONCE(pm_suspend_target_state) != PM_SUSPEND_TO_IDLE ||
	    irq != READ_ONCE(s2idle_grace_wake_irq))
		return false;

	return s2idle_grace_now_ns() < READ_ONCE(s2idle_grace_end_ns);
}
EXPORT_SYMBOL_GPL(pm_s2idle_grace_ignore_wakeup_irq);

static void s2idle_begin(void)
{
	s2idle_state = S2IDLE_STATE_NONE;
}

static void s2idle_enter(void)
{
	trace_suspend_resume(TPS("machine_suspend"), PM_SUSPEND_TO_IDLE, true);

	/*
	 * The correctness of the code below depends on the number of online
	 * CPUs being stable, but CPUs cannot be taken offline or put online
	 * while it is running.
	 *
	 * The s2idle_lock must be acquired before the pending wakeup check to
	 * prevent pm_system_wakeup() from running as a whole between that check
	 * and the subsequent s2idle_state update in which case a wakeup event
	 * would get lost.
	 */
	raw_spin_lock_irq(&s2idle_lock);
	if (pm_wakeup_pending())
		goto out;

	s2idle_state = S2IDLE_STATE_ENTER;
	raw_spin_unlock_irq(&s2idle_lock);

	/* Push all the CPUs into the idle loop. */
	wake_up_all_idle_cpus();
	/* Make the current CPU wait so it can enter the idle loop too. */
	swait_event_exclusive(s2idle_wait_head,
		    s2idle_state == S2IDLE_STATE_WAKE);

	/*
	 * Kick all CPUs to ensure that they resume their timers and restore
	 * consistent system state.
	 */
	wake_up_all_idle_cpus();

	raw_spin_lock_irq(&s2idle_lock);

 out:
	s2idle_state = S2IDLE_STATE_NONE;
	raw_spin_unlock_irq(&s2idle_lock);

	trace_suspend_resume(TPS("machine_suspend"), PM_SUSPEND_TO_IDLE, false);
}

static void s2idle_loop(void)
{
	pm_pr_dbg("suspend-to-idle\n");

	/*
	 * Suspend-to-idle equals:
	 * frozen processes + suspended devices + idle processors.
	 * Thus s2idle_enter() should be called right after all devices have
	 * been suspended.
	 *
	 * Wakeups during the noirq suspend of devices may be spurious, so try
	 * to avoid them upfront.
	 */
	for (;;) {
		if (s2idle_ops && s2idle_ops->wake) {
			if (s2idle_ops->wake())
				break;
		} else if (pm_wakeup_pending()) {
			break;
		}

		if (s2idle_ops && s2idle_ops->check)
			s2idle_ops->check();

		s2idle_enter();
	}

	pm_pr_dbg("resume from suspend-to-idle\n");
}

void s2idle_wake(void)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&s2idle_lock, flags);
	if (s2idle_state > S2IDLE_STATE_NONE) {
		s2idle_state = S2IDLE_STATE_WAKE;
		swake_up_one(&s2idle_wait_head);
	}
	raw_spin_unlock_irqrestore(&s2idle_lock, flags);
}
EXPORT_SYMBOL_GPL(s2idle_wake);

static bool valid_state(suspend_state_t state)
{
	/*
	 * The PM_SUSPEND_STANDBY and PM_SUSPEND_MEM states require low-level
	 * support and need to be valid to the low-level implementation.
	 *
	 * No ->valid() or ->enter() callback implies that none are valid.
	 */
	return suspend_ops && suspend_ops->valid && suspend_ops->valid(state) &&
		suspend_ops->enter;
}

void __init pm_states_init(void)
{
	/* "mem" and "freeze" are always present in /sys/power/state. */
	pm_states[PM_SUSPEND_MEM] = pm_labels[PM_SUSPEND_MEM];
	pm_states[PM_SUSPEND_TO_IDLE] = pm_labels[PM_SUSPEND_TO_IDLE];
	/*
	 * Suspend-to-idle should be supported even without any suspend_ops,
	 * initialize mem_sleep_states[] accordingly here.
	 */
	mem_sleep_states[PM_SUSPEND_TO_IDLE] = mem_sleep_labels[PM_SUSPEND_TO_IDLE];
}

static int __init mem_sleep_default_setup(char *str)
{
	suspend_state_t state;

	for (state = PM_SUSPEND_TO_IDLE; state <= PM_SUSPEND_MEM; state++)
		if (mem_sleep_labels[state] &&
		    !strcmp(str, mem_sleep_labels[state])) {
			mem_sleep_default = state;
			mem_sleep_current = state;
			break;
		}

	return 1;
}
__setup("mem_sleep_default=", mem_sleep_default_setup);

static void suspend_state_setup(void)
{
	if (valid_state(PM_SUSPEND_STANDBY)) {
		mem_sleep_states[PM_SUSPEND_STANDBY] = mem_sleep_labels[PM_SUSPEND_STANDBY];
		pm_states[PM_SUSPEND_STANDBY] = pm_labels[PM_SUSPEND_STANDBY];
		if (mem_sleep_default == PM_SUSPEND_STANDBY)
			mem_sleep_current = PM_SUSPEND_STANDBY;
	}
	if (valid_state(PM_SUSPEND_MEM)) {
		mem_sleep_states[PM_SUSPEND_MEM] = mem_sleep_labels[PM_SUSPEND_MEM];
		if (mem_sleep_default >= PM_SUSPEND_MEM)
			mem_sleep_current = PM_SUSPEND_MEM;
	}
}

/**
 * suspend_set_ops - Set the global suspend method table.
 * @ops: Suspend operations to use.
 */
void suspend_set_ops(const struct platform_suspend_ops *ops)
{
	unsigned int sleep_flags;

	sleep_flags = lock_system_sleep();

	suspend_ops = ops;
	suspend_state_setup();

	unlock_system_sleep(sleep_flags);
}
EXPORT_SYMBOL_GPL(suspend_set_ops);

/**
 * suspend_ops_is_set - Report whether any backend has registered a suspend op set.
 *
 * Used by alternative backends that want to register only when no other one is
 * already active (so they can coexist with the canonical PSCI SYSTEM_SUSPEND
 * path, which wins if firmware supports it).
 */
bool suspend_ops_is_set(void)
{
	bool set;
	unsigned int sleep_flags;

	sleep_flags = lock_system_sleep();
	set = suspend_ops != NULL;
	unlock_system_sleep(sleep_flags);

	return set;
}
EXPORT_SYMBOL_GPL(suspend_ops_is_set);

/**
 * suspend_set_ops_if_unused - Install @ops only if no other backend registered.
 * @ops: Suspend operations to install.
 *
 * Returns 0 on success (either @ops was installed or it was already the active
 * one), -EBUSY if a different backend already owns suspend_ops.
 */
int suspend_set_ops_if_unused(const struct platform_suspend_ops *ops)
{
	unsigned int sleep_flags;
	int ret = 0;

	sleep_flags = lock_system_sleep();

	if (suspend_ops && suspend_ops != ops) {
		ret = -EBUSY;
		goto out;
	}

	suspend_ops = ops;
	suspend_state_setup();

out:
	unlock_system_sleep(sleep_flags);
	return ret;
}
EXPORT_SYMBOL_GPL(suspend_set_ops_if_unused);

/**
 * suspend_valid_only_mem - Generic memory-only valid callback.
 * @state: Target system sleep state.
 *
 * Platform drivers that implement mem suspend only and only need to check for
 * that in their .valid() callback can use this instead of rolling their own
 * .valid() callback.
 */
int suspend_valid_only_mem(suspend_state_t state)
{
	return state == PM_SUSPEND_MEM;
}
EXPORT_SYMBOL_GPL(suspend_valid_only_mem);

static bool sleep_state_supported(suspend_state_t state)
{
	return state == PM_SUSPEND_TO_IDLE ||
	       (valid_state(state) && !cxl_mem_active());
}

static int platform_suspend_prepare(suspend_state_t state)
{
	return state != PM_SUSPEND_TO_IDLE && suspend_ops->prepare ?
		suspend_ops->prepare() : 0;
}

static int platform_suspend_prepare_late(suspend_state_t state)
{
	return state == PM_SUSPEND_TO_IDLE && s2idle_ops && s2idle_ops->prepare ?
		s2idle_ops->prepare() : 0;
}

static int platform_suspend_prepare_noirq(suspend_state_t state)
{
	if (state == PM_SUSPEND_TO_IDLE)
		return s2idle_ops && s2idle_ops->prepare_late ?
			s2idle_ops->prepare_late() : 0;

	return suspend_ops->prepare_late ? suspend_ops->prepare_late() : 0;
}

static void platform_resume_noirq(suspend_state_t state)
{
	if (state == PM_SUSPEND_TO_IDLE) {
		if (s2idle_ops && s2idle_ops->restore_early)
			s2idle_ops->restore_early();
	} else if (suspend_ops->wake) {
		suspend_ops->wake();
	}
}

static void platform_resume_early(suspend_state_t state)
{
	if (state == PM_SUSPEND_TO_IDLE && s2idle_ops && s2idle_ops->restore)
		s2idle_ops->restore();
}

static void platform_resume_finish(suspend_state_t state)
{
	if (state != PM_SUSPEND_TO_IDLE && suspend_ops->finish)
		suspend_ops->finish();
}

static int platform_suspend_begin(suspend_state_t state)
{
	if (state == PM_SUSPEND_TO_IDLE && s2idle_ops && s2idle_ops->begin)
		return s2idle_ops->begin();
	else if (suspend_ops && suspend_ops->begin)
		return suspend_ops->begin(state);
	else
		return 0;
}

static void platform_resume_end(suspend_state_t state)
{
	if (state == PM_SUSPEND_TO_IDLE && s2idle_ops && s2idle_ops->end)
		s2idle_ops->end();
	else if (suspend_ops && suspend_ops->end)
		suspend_ops->end();
}

static void platform_recover(suspend_state_t state)
{
	if (state != PM_SUSPEND_TO_IDLE && suspend_ops->recover)
		suspend_ops->recover();
}

static bool platform_suspend_again(suspend_state_t state)
{
	return state != PM_SUSPEND_TO_IDLE && suspend_ops->suspend_again ?
		suspend_ops->suspend_again() : false;
}

#ifdef CONFIG_PM_DEBUG
static unsigned int pm_test_delay = 5;
module_param(pm_test_delay, uint, 0644);
MODULE_PARM_DESC(pm_test_delay,
		 "Number of seconds to wait before resuming from suspend test");
#endif

static int suspend_test(int level)
{
#ifdef CONFIG_PM_DEBUG
	int i;

	if (pm_test_level == level) {
		pr_info("suspend debug: Waiting for %d second(s).\n",
				pm_test_delay);
		for (i = 0; i < pm_test_delay && !pm_wakeup_pending(); i++) {
			if (level > TEST_CORE)
				msleep(1000);
			else
				mdelay(1000);
		}
		return 1;
	}
#endif /* !CONFIG_PM_DEBUG */
	return 0;
}

/**
 * suspend_prepare - Prepare for entering system sleep state.
 * @state: Target system sleep state.
 *
 * Common code run for every system sleep state that can be entered (except for
 * hibernation).  Run suspend notifiers, allocate the "suspend" console and
 * freeze processes.
 */
static int suspend_prepare(suspend_state_t state)
{
	int error;

	if (!sleep_state_supported(state))
		return -EPERM;

	pm_prepare_console();

	error = pm_notifier_call_chain_robust(PM_SUSPEND_PREPARE, PM_POST_SUSPEND);
	if (error)
		goto Restore;

	filesystems_freeze(filesystem_freeze_enabled);
	trace_suspend_resume(TPS("freeze_processes"), 0, true);
	error = suspend_freeze_processes();
	trace_suspend_resume(TPS("freeze_processes"), 0, false);
	if (!error)
		return 0;

	dpm_save_failed_step(SUSPEND_FREEZE);
	filesystems_thaw();
	pm_notifier_call_chain(PM_POST_SUSPEND);
 Restore:
	pm_restore_console();
	return error;
}

/* default implementation */
void __weak arch_suspend_disable_irqs(void)
{
	local_irq_disable();
}

/* default implementation */
void __weak arch_suspend_enable_irqs(void)
{
	local_irq_enable();
}

/**
 * suspend_enter - Make the system enter the given sleep state.
 * @state: System sleep state to enter.
 * @wakeup: Returns information that the sleep state should not be re-entered.
 *
 * This function should be called after devices have been suspended.
 */
static int suspend_enter(suspend_state_t state, bool *wakeup)
{
	int error;

	error = platform_suspend_prepare(state);
	if (error)
		goto Platform_finish;

	error = dpm_suspend_late(PMSG_SUSPEND);
	if (error) {
		pr_err("late suspend of devices failed\n");
		goto Platform_finish;
	}
	error = platform_suspend_prepare_late(state);
	if (error)
		goto Devices_early_resume;

	error = dpm_suspend_noirq(PMSG_SUSPEND);
	if (error) {
		pr_err("noirq suspend of devices failed\n");
		goto Platform_early_resume;
	}
	error = platform_suspend_prepare_noirq(state);
	if (error)
		goto Platform_wake;

	if (suspend_test(TEST_PLATFORM))
		goto Platform_wake;

	if (state == PM_SUSPEND_TO_IDLE) {
		s2idle_loop();
		goto Platform_wake;
	}

	error = pm_sleep_disable_secondary_cpus();
	if (error || suspend_test(TEST_CPUS))
		goto Enable_cpus;

	arch_suspend_disable_irqs();
	BUG_ON(!irqs_disabled());

	system_state = SYSTEM_SUSPEND;

	error = syscore_suspend();
	if (!error) {
		*wakeup = pm_wakeup_pending();
		if (!(suspend_test(TEST_CORE) || *wakeup)) {
			trace_suspend_resume(TPS("machine_suspend"),
				state, true);
			error = suspend_ops->enter(state);
			trace_suspend_resume(TPS("machine_suspend"),
				state, false);
		} else if (*wakeup) {
			error = -EBUSY;
		}
		syscore_resume();
	}

	system_state = SYSTEM_RUNNING;

	arch_suspend_enable_irqs();
	BUG_ON(irqs_disabled());

 Enable_cpus:
	pm_sleep_enable_secondary_cpus();

 Platform_wake:
	platform_resume_noirq(state);
	dpm_resume_noirq(PMSG_RESUME);

 Platform_early_resume:
	platform_resume_early(state);

 Devices_early_resume:
	dpm_resume_early(PMSG_RESUME);

 Platform_finish:
	platform_resume_finish(state);
	return error;
}

/**
 * suspend_devices_and_enter - Suspend devices and enter system sleep state.
 * @state: System sleep state to enter.
 */
int suspend_devices_and_enter(suspend_state_t state)
{
	int error;
	bool wakeup = false;

	if (!sleep_state_supported(state))
		return -ENOSYS;

	pm_suspend_target_state = state;

	if (state == PM_SUSPEND_TO_IDLE)
		pm_set_suspend_no_platform();

	error = platform_suspend_begin(state);
	if (error)
		goto Close;

	console_suspend_all();
	suspend_test_start();
	error = dpm_suspend_start(PMSG_SUSPEND);
	if (error) {
		pr_err("Some devices failed to suspend, or early wake event detected\n");
		goto Recover_platform;
	}
	suspend_test_finish("suspend devices");
	if (suspend_test(TEST_DEVICES))
		goto Recover_platform;

	do {
		error = suspend_enter(state, &wakeup);
	} while (!error && !wakeup && platform_suspend_again(state));

 Resume_devices:
	suspend_test_start();
	dpm_resume_end(PMSG_RESUME);
	suspend_test_finish("resume devices");
	trace_suspend_resume(TPS("console_resume_all"), state, true);
	console_resume_all();
	trace_suspend_resume(TPS("console_resume_all"), state, false);

 Close:
	platform_resume_end(state);
	pm_suspend_target_state = PM_SUSPEND_ON;
	return error;

 Recover_platform:
	platform_recover(state);
	goto Resume_devices;
}

/**
 * suspend_finish - Clean up before finishing the suspend sequence.
 *
 * Call platform code to clean up, restart processes, and free the console that
 * we've allocated. This routine is not called for hibernation.
 */
static void suspend_finish(void)
{
	suspend_thaw_processes();
	filesystems_thaw();
	pm_notifier_call_chain(PM_POST_SUSPEND);
	pm_restore_console();
}

/**
 * enter_state - Do common work needed to enter system sleep state.
 * @state: System sleep state to enter.
 *
 * Make sure that no one else is trying to put the system into a sleep state.
 * Fail if that's not the case.  Otherwise, prepare for system suspend, make the
 * system enter the given sleep state and clean up after wakeup.
 */
static int enter_state(suspend_state_t state)
{
	int error;

	trace_suspend_resume(TPS("suspend_enter"), state, true);
	if (state == PM_SUSPEND_TO_IDLE) {
#ifdef CONFIG_PM_DEBUG
		if (pm_test_level != TEST_NONE && pm_test_level <= TEST_CPUS) {
			pr_warn("Unsupported test mode for suspend to idle, please choose none/freezer/devices/platform.\n");
			return -EAGAIN;
		}
#endif
	} else if (!valid_state(state)) {
		return -EINVAL;
	}
	if (!mutex_trylock(&system_transition_mutex))
		return -EBUSY;

	if (state == PM_SUSPEND_TO_IDLE)
		s2idle_begin();

	if (sync_on_suspend_enabled) {
		trace_suspend_resume(TPS("sync_filesystems"), 0, true);

		error = pm_sleep_fs_sync();
		if (error)
			goto Unlock;

		trace_suspend_resume(TPS("sync_filesystems"), 0, false);
	}

	pm_pr_dbg("Preparing system for sleep (%s)\n", mem_sleep_labels[state]);
	pm_suspend_clear_flags();
	error = suspend_prepare(state);
	if (error)
		goto Unlock;

	if (suspend_test(TEST_FREEZER))
		goto Finish;

	trace_suspend_resume(TPS("suspend_enter"), state, false);
	pm_pr_dbg("Suspending system (%s)\n", mem_sleep_labels[state]);
	error = suspend_devices_and_enter(state);

 Finish:
	events_check_enabled = false;
	pm_pr_dbg("Finishing wakeup.\n");
	suspend_finish();
 Unlock:
	mutex_unlock(&system_transition_mutex);
	return error;
}

/**
 * pm_suspend - Externally visible function for suspending the system.
 * @state: System sleep state to enter.
 *
 * Check if the value of @state represents one of the supported states,
 * execute enter_state() and update system suspend statistics.
 */
int pm_suspend(suspend_state_t state)
{
	int error;

	if (state <= PM_SUSPEND_ON || state >= PM_SUSPEND_MAX)
		return -EINVAL;

	pr_info("suspend entry (%s)\n", mem_sleep_labels[state]);
	error = enter_state(state);
	dpm_save_errno(error);
	pr_info("suspend exit\n");
	return error;
}
EXPORT_SYMBOL(pm_suspend);
