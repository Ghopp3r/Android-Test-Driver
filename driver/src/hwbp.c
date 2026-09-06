// SPDX-License-Identifier: GPL-2.0-only
// AArch64 user execute-breakpoint overrides and hit capture.

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/hw_breakpoint.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/list.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/mmap_lock.h>
#include <linux/mutex.h>
#include <linux/path.h>
#include <linux/perf_event.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#include <asm/cputype.h>
#include <asm/sysreg.h>

#include <asm/compat.h>
#include <asm/cpufeature.h>
#include <asm/fpsimd.h>
#include <asm/memory.h>
#include <asm/processor.h>
#include <asm/uaccess.h>

#include <driver/uapi.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
#include <linux/cfi.h>
#endif
#ifndef __nocfi
#define __nocfi
#endif

#include "hwbp.h"
#include "kallsym.h"
#include "log.h"

typedef struct perf_event *(*drv_register_user_hw_bp_fn_t)(struct perf_event_attr *attr, perf_overflow_handler_t handler, void *context, struct task_struct *task);
typedef void (*drv_unregister_hw_bp_fn_t)(struct perf_event *bp);
typedef int (*drv_modify_user_hw_bp_fn_t)(struct perf_event *bp, struct perf_event_attr *attr);
typedef int (*drv_access_remote_vm_fn_t)(struct mm_struct *mm, unsigned long addr, void *buf, int len, unsigned int gup_flags);
typedef void (*drv_fpsimd_preserve_current_state_fn_t)(void);
typedef void (*drv_fpsimd_update_current_state_fn_t)(const struct user_fpsimd_state *state);

static drv_register_user_hw_bp_fn_t drv_register_user_hw_bp_ptr;
static drv_unregister_hw_bp_fn_t drv_unregister_hw_bp_ptr;
static drv_modify_user_hw_bp_fn_t drv_modify_user_hw_bp_ptr;
static drv_access_remote_vm_fn_t drv_access_remote_vm_ptr;
static drv_fpsimd_preserve_current_state_fn_t drv_fpsimd_preserve_current_state_ptr;
static drv_fpsimd_update_current_state_fn_t drv_fpsimd_update_current_state_ptr;
static bool hwbp_initialized;
static bool hwbp_ready;
static bool hwbp_fp_ready;

static __nocfi noinline struct perf_event *drv_call_register_user_hw_bp(drv_register_user_hw_bp_fn_t fn, struct perf_event_attr *attr, perf_overflow_handler_t handler, void *context, struct task_struct *task) { return fn(attr, handler, context, task); }
static __nocfi noinline void drv_call_unregister_hw_bp(drv_unregister_hw_bp_fn_t fn, struct perf_event *bp) { fn(bp); }
static __nocfi noinline int drv_call_modify_user_hw_bp(drv_modify_user_hw_bp_fn_t fn, struct perf_event *bp, struct perf_event_attr *attr) { return fn(bp, attr); }
static __nocfi noinline int drv_call_access_remote_vm(drv_access_remote_vm_fn_t fn, struct mm_struct *mm, unsigned long addr, void *buf, int len, unsigned int flags) { return fn(mm, addr, buf, len, flags); }
static __nocfi noinline void drv_call_fpsimd_preserve_current_state(drv_fpsimd_preserve_current_state_fn_t fn) { fn(); }
static __nocfi noinline void drv_call_fpsimd_update_current_state(drv_fpsimd_update_current_state_fn_t fn, const struct user_fpsimd_state *state) { fn(state); }

enum hwbp_toggle_state {
	HWBP_TOGGLE_ORIGIN,
	HWBP_TOGGLE_NEXT,
};

struct hwbp_tracker {
	struct list_head node;
	struct pid *pid_ref;
	struct mm_struct *mm;
	struct perf_event *bp;
	u64 addr;
	u32 bp_len;
	u32 bp_type;
	u32 pass_through;
	u32 orphaned;
	u32 flags;                   /* DRV_HWBP_FLAG_* — snapshot of install-time flags */
	struct file *owner_file;     /* fd-scoped cleanup: cleared when this fd is closed */
	spinlock_t override_lock;
	u32 override_count;
	struct drv_hwbp_reg_override overrides[DRV_HWBP_MAX_OVERRIDES];
	spinlock_t ring_lock;
	struct drv_hwbp_hit ring[DRV_HWBP_HIT_RING_SLOTS];
	u32 ring_head;
	u32 ring_tail;
	u32 ring_count;
	u64 ring_tail_seq;
	enum hwbp_toggle_state toggle;

	/* Sample gate: read lock-free in the handler; writer takes override_lock. */
	u32 sample_every;            /* 0 = every hit */
	u32 sample_counter;

	/* Condition gate: full tuple published atomically under override_lock. */
	u32 has_condition;
	u32 cond_op;                 /* DRV_HWBP_COND_* */
	u32 cond_reg;                /* 0..30 (X-reg index) */
	u64 cond_value;

	/* Bypass gate — one-shot; cleared via cmpxchg from the handler. */
	s32 bypass_pid;

	/* Async notify (E.HWBP.1) — notify_lock is raw so the handler can take it. */
	raw_spinlock_t notify_lock;
	struct pid *notify_pid_ref;
	int notify_signal;           /* 0 = HWBP_DEFAULT_SIGNAL */
	u32 notify_in_flight;        /* single outstanding worker */
	u32 tracker_id;              /* stable id for si_int payload */

	/* Watchpoint one-shot disable (R1). ARM64 arch hw-bp core only auto-steps
	 * events whose overflow handler is the default; we install a custom one,
	 * so a watchpoint that fires would re-fire forever on the same access.
	 * The handler queues a deferred modify_user_hw_breakpoint(disabled=1)
	 * once per fire (cmpxchg debounces re-entrance). Clients re-arm by
	 * calling install() again on the same (pid, addr). */
	u32 wp_disable_pending;
};

/* Kernel-side default RT signal. 43 is safely past Bionic's reserved window
 * (32..34 libc + 40/41 android_run_on_all_threads on Android 15/16). */
#define HWBP_DEFAULT_SIGNAL 43

static LIST_HEAD(hwbp_trackers);
static DEFINE_MUTEX(hwbp_mutex);
static atomic_t hwbp_tracker_id_seq = ATOMIC_INIT(1);

/* HW cap snapshot (populated once in hwbp_init from ID_AA64DFR0_EL1). */
static u32 hwbp_num_brps;
static u32 hwbp_num_wrps;

struct hwbp_notify_work {
	struct work_struct work;
	struct pid *pid;
	int signal_no;
	int bp_id;
};

/* Watchpoint one-shot disable — see hwbp_wp_schedule_disable(). */
struct hwbp_disable_work {
	struct work_struct work;
	u32 tracker_id;
};

/* Forward declarations for helpers used inside the handler. */
static unsigned long translate_bait(struct mm_struct *mm, unsigned long addr);
static void hwbp_notify_worker(struct work_struct *w);
static void hwbp_wp_disable_worker(struct work_struct *w);

static int hwbp_validate_override(const struct drv_hwbp_reg_override *override) {
	if (!override)
		return -EINVAL;

	switch (override->kind) {
		case DRV_HWBP_REG_NONE:
			return 0;
		case DRV_HWBP_REG_X:
			return override->index <= 30u ? 0 : -EINVAL;
		case DRV_HWBP_REG_VLO:
		case DRV_HWBP_REG_VHI:
			if (!hwbp_fp_ready)
				return -EOPNOTSUPP;
			return override->index <= 31u ? 0 : -EINVAL;
		case DRV_HWBP_REG_PC:
			return override->index == 0 ? 0 : -EINVAL;
		default:
			return -EINVAL;
	}
}

static int hwbp_validate_overrides(const struct drv_hwbp_install_req *req) {
	u32 i;

	if (req->override_count > DRV_HWBP_MAX_OVERRIDES)
		return -EINVAL;
	for (i = 0; i < req->override_count; i++) {
		int rc = hwbp_validate_override(&req->overrides[i]);

		if (rc)
			return rc;
	}
	return 0;
}

static bool hwbp_request_has_pc_override(const struct drv_hwbp_install_req *req) {
	u32 i;

	for (i = 0; i < req->override_count; i++) {
		if (req->overrides[i].kind == DRV_HWBP_REG_PC)
			return true;
	}
	return false;
}

static bool hwbp_overrides_have_pc(const struct drv_hwbp_reg_override *overrides, u32 count) {
	u32 i;

	for (i = 0; i < count; i++) {
		if (overrides[i].kind == DRV_HWBP_REG_PC)
			return true;
	}
	return false;
}

static void hwbp_set_overrides(struct hwbp_tracker *tracker, const struct drv_hwbp_install_req *req) {
	unsigned long flags;

	spin_lock_irqsave(&tracker->override_lock, flags);
	tracker->override_count = req->override_count;
	memcpy(tracker->overrides, req->overrides, sizeof(tracker->overrides));
	spin_unlock_irqrestore(&tracker->override_lock, flags);
}

static u32 hwbp_snapshot_overrides(struct hwbp_tracker *tracker, struct drv_hwbp_reg_override *out) {
	unsigned long flags;
	u32 count;

	spin_lock_irqsave(&tracker->override_lock, flags);
	count = tracker->override_count;
	memcpy(out, tracker->overrides, sizeof(tracker->overrides));
	spin_unlock_irqrestore(&tracker->override_lock, flags);
	return count;
}

static void hwbp_ring_push(struct hwbp_tracker *tracker, const struct pt_regs *regs) {
	struct drv_hwbp_hit *hit;
	unsigned long flags;
	u32 i;
	bool want_fp = (tracker->flags & DRV_HWBP_FLAG_CAPTURE_FP) && hwbp_fp_ready;

	/* FPSIMD save must run before the ring spinlock — the helper may sleep on 5.10 (SVE flush). */
	struct user_fpsimd_state fp_snap;
	if (want_fp) {
		drv_call_fpsimd_preserve_current_state(drv_fpsimd_preserve_current_state_ptr);
		fp_snap = current->thread.uw.fpsimd_state;
	}

	spin_lock_irqsave(&tracker->ring_lock, flags);
	hit = &tracker->ring[tracker->ring_head];
	memset(hit, 0, sizeof(*hit));
	hit->timestamp_ns = ktime_get_boottime_ns();
	hit->pc = regs->pc;
	hit->sp = regs->sp;
	hit->pstate = regs->pstate;
	for (i = 0; i < ARRAY_SIZE(hit->x); i++)
		hit->x[i] = regs->regs[i];
	if (want_fp) {
		for (i = 0; i < 32; i++) {
			/* vregs[i] is __uint128_t: low 8B = D[i].low, high 8B = D[i].high */
			hit->q_lo[i] = (u64)fp_snap.vregs[i];
			hit->q_hi[i] = (u64)(fp_snap.vregs[i] >> 64);
		}
		hit->fpsr = fp_snap.fpsr;
		hit->fpcr = fp_snap.fpcr;
	}
	tracker->ring_head = (tracker->ring_head + 1u) % DRV_HWBP_HIT_RING_SLOTS;
	if (tracker->ring_count < DRV_HWBP_HIT_RING_SLOTS) {
		tracker->ring_count++;
	} else {
		tracker->ring_tail = (tracker->ring_tail + 1u) % DRV_HWBP_HIT_RING_SLOTS;
		tracker->ring_tail_seq++;
	}
	spin_unlock_irqrestore(&tracker->ring_lock, flags);
}

/* Takes a consistent snapshot of the (reg, op, value, active) tuple under
 * override_lock — the setter publishes them all under the same lock, so the
 * handler never mixes fields from two different rule versions (bug #10). */
static bool hwbp_condition_matches(struct hwbp_tracker *tracker, const struct pt_regs *regs) {
	unsigned long flags;
	u32 active, reg, op;
	u64 want, v;

	spin_lock_irqsave(&tracker->override_lock, flags);
	active = tracker->has_condition;
	reg = tracker->cond_reg;
	op = tracker->cond_op;
	want = tracker->cond_value;
	spin_unlock_irqrestore(&tracker->override_lock, flags);

	if (!active)
		return true;
	if (reg > 30u)
		return true;
	v = regs->regs[reg];
	switch (op) {
	case DRV_HWBP_COND_EQ: return v == want;
	case DRV_HWBP_COND_NE: return v != want;
	case DRV_HWBP_COND_LT: return (s64)v <  (s64)want;
	case DRV_HWBP_COND_LE: return (s64)v <= (s64)want;
	case DRV_HWBP_COND_GT: return (s64)v >  (s64)want;
	case DRV_HWBP_COND_GE: return (s64)v >= (s64)want;
	default:               return true;
	}
}

static void hwbp_apply_gp(struct pt_regs *regs, const struct drv_hwbp_reg_override *overrides, u32 count) {
	u32 i;

	for (i = 0; i < count; i++) {
		switch (overrides[i].kind) {
			case DRV_HWBP_REG_X:
				regs->regs[overrides[i].index] = overrides[i].value;
				break;
			case DRV_HWBP_REG_PC:
				regs->pc = overrides[i].value;
				break;
			default:
				break;
		}
	}
}

static void hwbp_apply_fp(const struct drv_hwbp_reg_override *overrides, u32 count) {
	struct user_fpsimd_state state;
	u64 *vregs;
	u32 i;

	if (!hwbp_fp_ready)
		return;
	drv_call_fpsimd_preserve_current_state(drv_fpsimd_preserve_current_state_ptr);
	state = current->thread.uw.fpsimd_state;
	vregs = (u64 *)&state.vregs[0];
	for (i = 0; i < count; i++) {
		if (overrides[i].kind == DRV_HWBP_REG_VLO)
			vregs[2u * overrides[i].index] = overrides[i].value;
		else if (overrides[i].kind == DRV_HWBP_REG_VHI)
			vregs[2u * overrides[i].index + 1u] = overrides[i].value;
	}
	drv_call_fpsimd_update_current_state(drv_fpsimd_update_current_state_ptr, &state);
}

static bool hwbp_has_fp_override(const struct drv_hwbp_reg_override *overrides, u32 count) {
	u32 i;

	for (i = 0; i < count; i++) {
		if (overrides[i].kind == DRV_HWBP_REG_VLO || overrides[i].kind == DRV_HWBP_REG_VHI)
			return true;
	}
	return false;
}

static int hwbp_set_breakpoint_address(struct perf_event *bp, u64 addr) {
	struct perf_event_attr attr = bp->attr;

	attr.bp_addr = addr;
	attr.disabled = 0;
	return drv_call_modify_user_hw_bp(drv_modify_user_hw_bp_ptr, bp, &attr);
}

static void hwbp_disable_orphaned(struct hwbp_tracker *tracker, struct perf_event *bp) {
	struct perf_event_attr attr = bp->attr;
	int rc;

	WRITE_ONCE(tracker->orphaned, 1u);
	attr.disabled = 1;
	rc = drv_call_modify_user_hw_bp(drv_modify_user_hw_bp_ptr, bp, &attr);
	if (rc)
		LOGW_RL("hwbp: disable stale tracker rc=%d\n", rc);
}

/* Snapshots the notify target under notify_lock so setter's put_pid can never
 * race with the handler picking up a freed pid. Bug #3. */
static void hwbp_maybe_notify(struct hwbp_tracker *tracker) {
	struct hwbp_notify_work *nw;
	struct pid *pid_ref = NULL;
	int signal_no;
	unsigned long flags;

	if (!(tracker->flags & DRV_HWBP_FLAG_NOTIFY))
		return;
	if (cmpxchg(&tracker->notify_in_flight, 0u, 1u) != 0u)
		return;

	raw_spin_lock_irqsave(&tracker->notify_lock, flags);
	if (tracker->notify_pid_ref)
		pid_ref = get_pid(tracker->notify_pid_ref);
	signal_no = tracker->notify_signal;
	raw_spin_unlock_irqrestore(&tracker->notify_lock, flags);

	if (!pid_ref) {
		WRITE_ONCE(tracker->notify_in_flight, 0u);
		return;
	}
	nw = kmalloc(sizeof(*nw), GFP_ATOMIC);
	if (!nw) {
		put_pid(pid_ref);
		WRITE_ONCE(tracker->notify_in_flight, 0u);
		return;
	}
	nw->pid = pid_ref;
	nw->signal_no = signal_no ? signal_no : HWBP_DEFAULT_SIGNAL;
	nw->bp_id = (int)tracker->tracker_id;
	INIT_WORK(&nw->work, hwbp_notify_worker);
	queue_work(system_wq, &nw->work);
}

/* Advances past the trapped instruction for a non-pass_through execute BP by
 * LR-return — the caller resumes as if the function had returned. Watchpoints
 * cannot use this path (their trap point is inside a load/store), so R/W/RW
 * breakpoints get one-shot semantics via hwbp_wp_schedule_disable() below. */
static void hwbp_execute_lr_return(struct pt_regs *regs) {
	regs->pc = regs->regs[30];
}

/* Queues an asynchronous "disable this tracker's perf event" — safe from the
 * overflow handler's atomic context because modify_user_hw_breakpoint takes a
 * mutex and can sleep. cmpxchg makes it idempotent per fire so a watchpoint
 * that traps a handful of times before the worker runs still queues once. */
static void hwbp_wp_schedule_disable(struct hwbp_tracker *tracker) {
	struct hwbp_disable_work *dw;

	if (cmpxchg(&tracker->wp_disable_pending, 0u, 1u) != 0u)
		return;
	dw = kmalloc(sizeof(*dw), GFP_ATOMIC);
	if (!dw) {
		WRITE_ONCE(tracker->wp_disable_pending, 0u);
		return;
	}
	dw->tracker_id = tracker->tracker_id;
	INIT_WORK(&dw->work, hwbp_wp_disable_worker);
	queue_work(system_wq, &dw->work);
}

/* Toggle-forward the execute BP: park it one insn ahead, mark NEXT so the
 * follow-up trap restores the original address. Used by both the success
 * path and by gate-skip when pass_through is set (bug #2). */
static int hwbp_advance_over_insn(struct hwbp_tracker *tracker, struct perf_event *bp) {
	int rc = hwbp_set_breakpoint_address(bp, tracker->addr + DRV_HWBP_LEN_EXECUTE);
	if (rc) {
		LOGW_RL("hwbp: advance rc=%d\n", rc);
		return rc;
	}
	WRITE_ONCE(tracker->toggle, HWBP_TOGGLE_NEXT);
	return 0;
}

/* Recovers execution after a gate said "skip". Execute BPs LR-return or
 * advance the BP window depending on pass_through. Watchpoints route to the
 * same async-disable path as the success case — the arm64 core does NOT
 * auto-single-step our custom overflow handler, so leaving the event armed
 * loops on the same data access forever (R1). */
static void hwbp_recover_after_skip(struct hwbp_tracker *tracker, struct perf_event *bp,
                                    struct pt_regs *regs) {
	if (tracker->bp_type != DRV_HWBP_TYPE_X) {
		hwbp_wp_schedule_disable(tracker);
		return;
	}
	if (tracker->pass_through)
		hwbp_advance_over_insn(tracker, bp);
	else
		hwbp_execute_lr_return(regs);
}

static void hwbp_handler(struct perf_event *bp, struct perf_sample_data *data, struct pt_regs *regs) {
	struct hwbp_tracker *tracker;
	struct drv_hwbp_reg_override overrides[DRV_HWBP_MAX_OVERRIDES];
	u32 count;
	int rc;
	bool timing_bypass;

	(void)data;
	if (!bp || !regs)
		return;
	tracker = bp->overflow_handler_context;
	if (!tracker)
		return;
	if (unlikely(READ_ONCE(tracker->orphaned) || current->mm != tracker->mm)) {
		hwbp_disable_orphaned(tracker, bp);
		return;
	}

	/* pass_through toggle: second hit is our own re-arm, restore origin address and exit. */
	if (tracker->pass_through && READ_ONCE(tracker->toggle) == HWBP_TOGGLE_NEXT) {
		rc = hwbp_set_breakpoint_address(bp, tracker->addr);
		if (rc) {
			LOGW_RL("hwbp: rearm rc=%d\n", rc);
			return;
		}
		WRITE_ONCE(tracker->toggle, HWBP_TOGGLE_ORIGIN);
		return;
	}

	/* Bypass gate — one-shot silent consume. */
	{
		s32 by = READ_ONCE(tracker->bypass_pid);
		if (by && by == (s32)current->tgid) {
			cmpxchg(&tracker->bypass_pid, by, 0);
			hwbp_recover_after_skip(tracker, bp, regs);
			return;
		}
	}

	/* Sample gate — count every hit, only fire when the counter divides. */
	{
		u32 every = READ_ONCE(tracker->sample_every);
		if (every) {
			u32 c = ++tracker->sample_counter;
			if (c % every != 0) {
				hwbp_recover_after_skip(tracker, bp, regs);
				return;
			}
		}
	}

	/* Condition gate — skip if the {reg, op, value} rule doesn't match. */
	if (!hwbp_condition_matches(tracker, regs)) {
		hwbp_recover_after_skip(tracker, bp, regs);
		return;
	}

	/* TIMING_BYPASS: skip ring push + notify; register overrides still applied. */
	timing_bypass = !!(tracker->flags & DRV_HWBP_FLAG_TIMING_BYPASS);

	count = hwbp_snapshot_overrides(tracker, overrides);
	if (!timing_bypass) {
		hwbp_ring_push(tracker, regs);
		hwbp_maybe_notify(tracker);
	}
	if (hwbp_has_fp_override(overrides, count))
		hwbp_apply_fp(overrides, count);
	hwbp_apply_gp(regs, overrides, count);

	/* Watchpoint success: one-shot semantics. We can't LR-return (the trap
	 * sits mid-load/store) and the arm64 core doesn't auto-step our custom
	 * overflow handler, so we defer a modify_user_hw_breakpoint(disabled=1).
	 * Clients re-arm by calling install() on the same (pid, addr). */
	if (tracker->bp_type != DRV_HWBP_TYPE_X) {
		hwbp_wp_schedule_disable(tracker);
		return;
	}

	if (!tracker->pass_through) {
		if (!hwbp_overrides_have_pc(overrides, count))
			hwbp_execute_lr_return(regs);
		return;
	}

	hwbp_advance_over_insn(tracker, bp);
}
NOKPROBE_SYMBOL(hwbp_handler);

/* Process-directed delivery (N4): pick a thread whose signal mask does NOT
 * block sig, and send exactly one si_int-carrying signal there. Falls back
 * to the group leader if every thread has it blocked — the signal stays
 * pending on the leader's private queue until it unblocks, same as any
 * queued process-directed RT signal. That's the shape signal(7) documents,
 * with the one caveat that send_sig_info can't do PIDTYPE_TGID for us so
 * we synthesise the target choice by walking t->blocked. */
static void hwbp_notify_worker(struct work_struct *w) {
	struct hwbp_notify_work *nw = container_of(w, struct hwbp_notify_work, work);
	struct task_struct *leader, *t, *chosen = NULL;
	struct kernel_siginfo info;
	int rc = -ESRCH;

	if (!nw->pid)
		goto out;

	memset(&info, 0, sizeof(info));
	info.si_signo = nw->signal_no;
	info.si_code = SI_QUEUE;
	info.si_int = nw->bp_id;

	leader = get_pid_task(nw->pid, PIDTYPE_TGID);
	if (!leader)
		leader = get_pid_task(nw->pid, PIDTYPE_PID);
	if (leader) {
		rcu_read_lock();
		for_each_thread(leader, t) {
			/* lock_task_sighand() atomically checks t->sighand under RCU
			 * and takes siglock — a bare `t->sighand ? spin_lock(...) : ..`
			 * races with release_task() clearing sighand under our feet
			 * once a thread starts exiting. Skip exiting/reaped threads. */
			unsigned long flags;
			bool blocked;
			struct sighand_struct *sh = lock_task_sighand(t, &flags);
			if (!sh)
				continue;
			blocked = sigismember(&t->blocked, nw->signal_no);
			unlock_task_sighand(t, &flags);
			if (!blocked) { chosen = t; break; }
		}
		if (!chosen)
			chosen = leader; /* every thread blocks it — queue on leader */
		get_task_struct(chosen);
		rcu_read_unlock();
		rc = send_sig_info(nw->signal_no, &info, chosen);
		put_task_struct(chosen);
		put_task_struct(leader);
	}
	if (rc)
		LOGW_RL("hwbp: notify deliver sig=%d rc=%d\n", nw->signal_no, rc);

	/* Best-effort in_flight clear — tracker may already be gone. */
	{
		struct hwbp_tracker *tr;
		mutex_lock(&hwbp_mutex);
		list_for_each_entry(tr, &hwbp_trackers, node) {
			if ((int)tr->tracker_id == nw->bp_id) {
				WRITE_ONCE(tr->notify_in_flight, 0u);
				break;
			}
		}
		mutex_unlock(&hwbp_mutex);
	}

	put_pid(nw->pid);
out:
	kfree(nw);
}

/* Deferred one-shot disable for a watchpoint tracker (R1). Called under the
 * global mutex so the perf_event pointer we hand to modify_user_hw_breakpoint
 * can't be freed underneath us. Skips if the tracker was removed, was
 * already orphaned by another path, or had its pending flag cleared by a
 * concurrent re-install (N3 — re-arm cancels the queued disable). */
static void hwbp_wp_disable_worker(struct work_struct *w) {
	struct hwbp_disable_work *dw = container_of(w, struct hwbp_disable_work, work);
	struct hwbp_tracker *tr;

	mutex_lock(&hwbp_mutex);
	list_for_each_entry(tr, &hwbp_trackers, node) {
		if (tr->tracker_id != dw->tracker_id)
			continue;
		if (!READ_ONCE(tr->wp_disable_pending))
			break; /* re-arm cancelled us */
		if (tr->bp && !READ_ONCE(tr->orphaned))
			hwbp_disable_orphaned(tr, tr->bp);
		WRITE_ONCE(tr->wp_disable_pending, 0u);
		break;
	}
	mutex_unlock(&hwbp_mutex);
	kfree(dw);
}

/* fd-scoped lookup — trackers form a (pid, addr, owner_file) key, so two
 * clients that pick the same target address get two independent trackers
 * and can't step on each other's state (bug #7). Passing owner=NULL falls
 * back to the legacy (pid, addr) match — used by translate_bait / caps /
 * clear-all-owned-by-file paths that don't have a single owner. */
static struct hwbp_tracker *hwbp_lookup_locked(struct pid *pid_ref, u64 addr,
                                               struct file *owner) {
	struct hwbp_tracker *tracker;

	list_for_each_entry(tracker, &hwbp_trackers, node) {
		if (tracker->pid_ref != pid_ref || tracker->addr != addr)
			continue;
		if (owner && tracker->owner_file != owner)
			continue;
		return tracker;
	}
	return NULL;
}

static void hwbp_tracker_free(struct hwbp_tracker *tracker) {
	if (!tracker)
		return;
	if (tracker->mm)
		mmdrop(tracker->mm);
	if (tracker->pid_ref)
		put_pid(tracker->pid_ref);
	if (tracker->notify_pid_ref)
		put_pid(tracker->notify_pid_ref);
	kfree(tracker);
}

static void hwbp_unregister_and_free(struct hwbp_tracker *tracker) {
	if (tracker->bp)
		drv_call_unregister_hw_bp(drv_unregister_hw_bp_ptr, tracker->bp);
	hwbp_tracker_free(tracker);
}

static int hwbp_validate_address(struct mm_struct *mm, unsigned long addr, u32 bp_type, u32 bp_len) {
	struct vm_area_struct *vma;
	int rc = 0;

	/* X aligns to 4B (single insn); watchpoints align to bp_len (1/2/4/8). */
	u32 align = (bp_type == DRV_HWBP_TYPE_X) ? DRV_HWBP_LEN_4 : bp_len;

	if (!addr || (addr & (align - 1u)) || addr > ULONG_MAX - bp_len)
		return -EINVAL;
	mmap_read_lock(mm);
	vma = find_vma(mm, addr);
	if (!vma || addr < vma->vm_start || addr + bp_len > vma->vm_end || (vma->vm_flags & (VM_IO | VM_PFNMAP)))
		rc = -EFAULT;
	else if (bp_type == DRV_HWBP_TYPE_X && !(vma->vm_flags & VM_EXEC))
		rc = -EFAULT;
	mmap_read_unlock(mm);
	return rc;
}

static bool hwbp_is_control_flow(u32 insn) {
	if ((insn & 0x7C000000u) == 0x14000000u)
		return true;
	if ((insn & 0xFF000010u) == 0x54000000u)
		return true;
	if ((insn & 0x7E000000u) == 0x34000000u || (insn & 0x7E000000u) == 0x36000000u)
		return true;
	if ((insn & 0xFFE00000u) == 0xD4000000u || (insn & 0xFE000000u) == 0xD6000000u)
		return true;
	return false;
}

static int hwbp_validate_fallthrough(struct mm_struct *mm, unsigned long addr) {
	u32 insn;
	int rc;

	rc = drv_call_access_remote_vm(drv_access_remote_vm_ptr, mm, addr, &insn, sizeof(insn), FOLL_FORCE);
	if (rc != sizeof(insn))
		return rc < 0 ? rc : -EFAULT;
	return hwbp_is_control_flow(insn) ? -EOPNOTSUPP : 0;
}

static int hwbp_get_task_mm(struct pid *pid_ref, struct task_struct **out_task, struct mm_struct **out_mm) {
	struct task_struct *task;
	struct mm_struct *mm;

	*out_task = NULL;
	*out_mm = NULL;
	task = get_pid_task(pid_ref, PIDTYPE_PID);
	if (!task)
		return -ESRCH;
#if IS_ENABLED(CONFIG_COMPAT)
	if (is_compat_thread(task_thread_info(task))) {
		put_task_struct(task);
		return -EOPNOTSUPP;
	}
#endif
	mm = get_task_mm(task);
	if (!mm) {
		put_task_struct(task);
		return -ESRCH;
	}
	*out_task = task;
	*out_mm = mm;
	return 0;
}

static int hwbp_get_pid_ref(u64 raw_pid, struct pid **out_pid) {
	if (!raw_pid || raw_pid > INT_MAX)
		return -EINVAL;
	*out_pid = find_get_pid((pid_t)raw_pid);
	return *out_pid ? 0 : -ESRCH;
}

static int hwbp_normalize_install(struct drv_hwbp_install_req *req) {
	int rc;

	if (req->pid <= 0)
		return -EINVAL;
	req->addr = (u64)untagged_addr((unsigned long)req->addr);
	if (!req->bp_type)
		req->bp_type = DRV_HWBP_TYPE_X;
	if (!req->bp_len)
		req->bp_len = (req->bp_type == DRV_HWBP_TYPE_X) ? DRV_HWBP_LEN_4 : DRV_HWBP_LEN_4;

	/* ARMv8 debug spec: X requires len=4; R/W/RW allow 1/2/4/8. */
	switch (req->bp_type) {
	case DRV_HWBP_TYPE_R:
	case DRV_HWBP_TYPE_W:
	case DRV_HWBP_TYPE_RW:
		if (req->bp_len != DRV_HWBP_LEN_1 && req->bp_len != DRV_HWBP_LEN_2 &&
		    req->bp_len != DRV_HWBP_LEN_4 && req->bp_len != DRV_HWBP_LEN_8)
			return -EOPNOTSUPP;
		/* pass_through is X-only: watchpoint data access does not advance PC. */
		if (req->pass_through)
			return -EOPNOTSUPP;
		break;
	case DRV_HWBP_TYPE_X:
		if (req->bp_len != DRV_HWBP_LEN_4)
			return -EOPNOTSUPP;
		break;
	default:
		return -EOPNOTSUPP;
	}
	if (req->pass_through > 1u)
		return -EOPNOTSUPP;
	rc = hwbp_validate_overrides(req);
	if (rc)
		return rc;
	if (req->pass_through && hwbp_request_has_pc_override(req))
		return -EINVAL;
	return 0;
}

static long hwbp_install(void __user *arg, struct file *owner) {
	struct drv_hwbp_install_req req;
	struct hwbp_tracker *tracker;
	struct hwbp_tracker *existing;
	struct perf_event_attr attr;
	struct task_struct *task;
	struct mm_struct *mm;
	struct pid *pid_ref;
	int rc;

	if (copy_from_user(&req, arg, sizeof(req)) != 0)
		return -EFAULT;
	rc = hwbp_normalize_install(&req);
	if (rc)
		return rc;
	rc = hwbp_get_pid_ref((u64)req.pid, &pid_ref);
	if (rc)
		return rc;
	rc = hwbp_get_task_mm(pid_ref, &task, &mm);
	if (rc)
		goto out_put_pid;

	/* DRV_HWBP_FLAG_BAIT_GUARD is deprecated and no longer stored on the
	 * tracker. Its old redirect action is now exposed via the explicit
	 * DRV_CMD_HWBP_TRANSLATE_BAIT ioctl (see R5). */
	req.flags &= ~DRV_HWBP_FLAG_BAIT_GUARD;

	rc = hwbp_validate_address(mm, (unsigned long)req.addr, req.bp_type, req.bp_len);
	if (rc)
		goto out_put_task_mm;
	if (req.pass_through) {
		/* pass_through is X-only (see normalize); validate the fallthrough insn slot too. */
		rc = hwbp_validate_address(mm, (unsigned long)req.addr + DRV_HWBP_LEN_4, req.bp_type, req.bp_len);
		if (rc)
			goto out_put_task_mm;
		rc = hwbp_validate_fallthrough(mm, (unsigned long)req.addr);
		if (rc)
			goto out_put_task_mm;
	}

	mutex_lock(&hwbp_mutex);
	/* Only match against trackers this fd already owns; another client's
	 * tracker on the same (pid, addr) is left alone and we build our own. */
	existing = hwbp_lookup_locked(pid_ref, req.addr, owner);
	if (existing && existing->mm != mm) {
		/* The target exec'd between installs — the old tracker's mm belongs
		 * to a process that no longer exists, its perf_event is anchored to
		 * a dead task, and the arm64 hw-bp core would eventually orphan it
		 * on the next hit. Drop it now so the new install can build a
		 * tracker against the current mm instead of updating the stale one. */
		list_del(&existing->node);
		hwbp_unregister_and_free(existing);
		existing = NULL;
	}
	if (existing && existing->mm == mm) {
		if (existing->bp_type != req.bp_type || existing->bp_len != req.bp_len ||
		    existing->pass_through != req.pass_through) {
			rc = -EBUSY;
			mutex_unlock(&hwbp_mutex);
			goto out_put_task_mm;
		}
		/* Re-arm path (N3): re-installing on an orphaned tracker (either the
		 * watchpoint one-shot fired and the disable worker has already run,
		 * OR a worker is still pending) resurrects it in place. That keeps
		 * every sticky setter's state — notify_pid_ref, sample_every,
		 * condition, bypass_pid — instead of dropping it on the floor and
		 * silently recreating the object. */
		{
			struct perf_event_attr attr_reenable = existing->bp ? existing->bp->attr
			                                                    : (struct perf_event_attr){};
			bool was_orphaned = READ_ONCE(existing->orphaned);
			existing->flags = req.flags;
			hwbp_set_overrides(existing, &req);
			if (was_orphaned && existing->bp) {
				attr_reenable.disabled = 0;
				(void)drv_call_modify_user_hw_bp(drv_modify_user_hw_bp_ptr,
				                                 existing->bp, &attr_reenable);
				WRITE_ONCE(existing->orphaned, 0u);
			}
			/* Cancel any still-pending disable — it would race the re-arm
			 * and immediately re-orphan the tracker. flush_work runs under
			 * hwbp_mutex; the worker's list walk skips this tracker while
			 * we hold the mutex, so no deadlock. */
			WRITE_ONCE(existing->wp_disable_pending, 0u);
			existing->sample_counter = 0;
			rc = 0;
		}
		mutex_unlock(&hwbp_mutex);
		goto out_put_task_mm;
	}
	tracker = kzalloc(sizeof(*tracker), GFP_KERNEL);
	if (!tracker) {
		mutex_unlock(&hwbp_mutex);
		rc = -ENOMEM;
		goto out_put_task_mm;
	}
	INIT_LIST_HEAD(&tracker->node);
	spin_lock_init(&tracker->override_lock);
	spin_lock_init(&tracker->ring_lock);
	raw_spin_lock_init(&tracker->notify_lock);
	tracker->pid_ref = get_pid(pid_ref);
	tracker->mm = mm;
	mmgrab(mm);
	tracker->addr = req.addr;
	tracker->bp_len = req.bp_len;
	tracker->bp_type = req.bp_type;
	tracker->pass_through = req.pass_through;
	tracker->flags = req.flags;
	tracker->tracker_id = (u32)atomic_inc_return(&hwbp_tracker_id_seq);
	tracker->owner_file = owner;
	tracker->toggle = HWBP_TOGGLE_ORIGIN;
	hwbp_set_overrides(tracker, &req);
	hw_breakpoint_init(&attr);
	attr.bp_addr = req.addr;
	attr.bp_len = req.bp_len;
	/* DRV_HWBP_TYPE_* values are chosen to equal HW_BREAKPOINT_R/W/RW/X — forward as-is. */
	attr.bp_type = req.bp_type;
	attr.sample_period = 1;
	attr.exclude_kernel = 1;
	attr.exclude_hv = 1;
	tracker->bp = drv_call_register_user_hw_bp(drv_register_user_hw_bp_ptr, &attr, hwbp_handler, tracker, task);
	if (IS_ERR_OR_NULL(tracker->bp)) {
		rc = tracker->bp ? PTR_ERR(tracker->bp) : -EIO;
		tracker->bp = NULL;
		hwbp_tracker_free(tracker);
		mutex_unlock(&hwbp_mutex);
		goto out_put_task_mm;
	}
	list_add_tail(&tracker->node, &hwbp_trackers);
	mutex_unlock(&hwbp_mutex);
	LOGI("hwbp: installed pid=%d addr=%px type=%u len=%u passthrough=%u overrides=%u\n",
	     req.pid, (void *)(uintptr_t)req.addr, req.bp_type, req.bp_len, req.pass_through, req.override_count);
	rc = 0;

out_put_task_mm:
	mmput(mm);
	put_task_struct(task);
out_put_pid:
	put_pid(pid_ref);
	return rc;
}

static long hwbp_set_override(void __user *arg, struct file *owner) {
	struct drv_hwbp_install_req req;
	struct hwbp_tracker *tracker;
	struct task_struct *task;
	struct mm_struct *mm;
	struct pid *pid_ref;
	int rc;

	if (copy_from_user(&req, arg, sizeof(req)) != 0)
		return -EFAULT;
	if (req.pid <= 0)
		return -EINVAL;
	req.addr = (u64)untagged_addr((unsigned long)req.addr);
	rc = hwbp_validate_overrides(&req);
	if (rc)
		return rc;
	rc = hwbp_get_pid_ref((u64)req.pid, &pid_ref);
	if (rc)
		return rc;
	rc = hwbp_get_task_mm(pid_ref, &task, &mm);
	if (rc)
		goto out_put_pid;
	mutex_lock(&hwbp_mutex);
	tracker = hwbp_lookup_locked(pid_ref, req.addr, owner);
	if (!tracker)
		rc = -ENOENT;
	else if (tracker->mm != mm)
		rc = -ESTALE;
	else if (tracker->pass_through && hwbp_request_has_pc_override(&req))
		rc = -EINVAL;
	else {
		hwbp_set_overrides(tracker, &req);
		rc = 0;
	}
	mutex_unlock(&hwbp_mutex);
	mmput(mm);
	put_task_struct(task);
out_put_pid:
	put_pid(pid_ref);
	return rc;
}

static long hwbp_remove(void __user *arg, struct file *owner) {
	struct drv_ioctl_req req;
	struct hwbp_tracker *tracker;
	struct pid *pid_ref;
	int rc;

	if (copy_from_user(&req, arg, sizeof(req)) != 0)
		return -EFAULT;
	req.addr = (u64)untagged_addr((unsigned long)req.addr);
	rc = hwbp_get_pid_ref(req.pid, &pid_ref);
	if (rc)
		return rc;
	mutex_lock(&hwbp_mutex);
	tracker = hwbp_lookup_locked(pid_ref, req.addr, owner);
	if (!tracker) {
		mutex_unlock(&hwbp_mutex);
		put_pid(pid_ref);
		return -ENOENT;
	}
	list_del(&tracker->node);
	hwbp_unregister_and_free(tracker);
	mutex_unlock(&hwbp_mutex);
	put_pid(pid_ref);
	return 0;
}

static long hwbp_get_hits(void __user *arg, struct file *owner) {
	struct drv_ioctl_req req;
	struct hwbp_tracker *tracker;
	struct drv_hwbp_hit *hits;
	struct pid *pid_ref;
	unsigned long flags;
	u64 start_tail_seq;
	u64 written;
	u32 capacity;
	u32 count;
	u32 i;
	int rc;

	if (copy_from_user(&req, arg, sizeof(req)) != 0)
		return -EFAULT;
	if (!req.buf || req.size < sizeof(*hits))
		return -EINVAL;
	req.addr = (u64)untagged_addr((unsigned long)req.addr);
	rc = hwbp_get_pid_ref(req.pid, &pid_ref);
	if (rc)
		return rc;
	capacity = min_t(u64, req.size / sizeof(*hits), DRV_HWBP_HIT_RING_SLOTS);
	hits = kcalloc(capacity, sizeof(*hits), GFP_KERNEL);
	if (!hits) {
		put_pid(pid_ref);
		return -ENOMEM;
	}

	mutex_lock(&hwbp_mutex);
	tracker = hwbp_lookup_locked(pid_ref, req.addr, owner);
	if (!tracker) {
		rc = -ENOENT;
		goto out_unlock;
	}
	spin_lock_irqsave(&tracker->ring_lock, flags);
	count = min(tracker->ring_count, capacity);
	start_tail_seq = tracker->ring_tail_seq;
	for (i = 0; i < count; i++)
		hits[i] = tracker->ring[(tracker->ring_tail + i) % DRV_HWBP_HIT_RING_SLOTS];
	spin_unlock_irqrestore(&tracker->ring_lock, flags);
	written = (u64)count * sizeof(*hits);
	if (count && copy_to_user((void __user *)(uintptr_t)req.buf, hits, written) != 0) {
		rc = -EFAULT;
		goto out_unlock;
	}
	if (copy_to_user((u8 __user *)arg + offsetof(struct drv_ioctl_req, size), &written, sizeof(written)) != 0) {
		rc = -EFAULT;
		goto out_unlock;
	}
	spin_lock_irqsave(&tracker->ring_lock, flags);
	if (tracker->ring_tail_seq != start_tail_seq || tracker->ring_count < count) {
		spin_unlock_irqrestore(&tracker->ring_lock, flags);
		rc = -EAGAIN;
		goto out_unlock;
	}
	tracker->ring_tail = (tracker->ring_tail + count) % DRV_HWBP_HIT_RING_SLOTS;
	tracker->ring_count -= count;
	tracker->ring_tail_seq += count;
	/* Drained: re-arm the notify debouncer so the next hit signals again. */
	WRITE_ONCE(tracker->notify_in_flight, 0u);
	spin_unlock_irqrestore(&tracker->ring_lock, flags);
	rc = 0;

out_unlock:
	mutex_unlock(&hwbp_mutex);
	kfree(hits);
	put_pid(pid_ref);
	return rc;
}

/* Clears only the trackers owned by the calling fd — a runaway client can
 * no longer wipe out another client's state (bug #7). */
static long hwbp_clear_all(struct file *owner) {
	struct hwbp_tracker *tracker;
	struct hwbp_tracker *next;

	mutex_lock(&hwbp_mutex);
	list_for_each_entry_safe(tracker, next, &hwbp_trackers, node) {
		if (owner && tracker->owner_file != owner)
			continue;
		list_del(&tracker->node);
		hwbp_unregister_and_free(tracker);
	}
	mutex_unlock(&hwbp_mutex);
	return 0;
}

/* fd-scoped cleanup (A.2): called from .release, reclaims trackers owned by @f only. */
void hwbp_clear_by_file(struct file *f) {
	struct hwbp_tracker *tracker;
	struct hwbp_tracker *next;

	if (!f)
		return;
	mutex_lock(&hwbp_mutex);
	list_for_each_entry_safe(tracker, next, &hwbp_trackers, node) {
		if (tracker->owner_file == f) {
			list_del(&tracker->node);
			hwbp_unregister_and_free(tracker);
		}
	}
	mutex_unlock(&hwbp_mutex);
}

static long hwbp_get_caps(void __user *arg) {
	struct drv_hwbp_caps caps;
	memset(&caps, 0, sizeof(caps));
	caps.num_brps = hwbp_num_brps;
	caps.num_wrps = hwbp_num_wrps;
	caps.ring_slots = DRV_HWBP_HIT_RING_SLOTS;
	caps.max_overrides = DRV_HWBP_MAX_OVERRIDES;
	caps.hit_bytes = sizeof(struct drv_hwbp_hit);
	caps.install_req_bytes = sizeof(struct drv_hwbp_install_req);
	/* BAIT_GUARD intentionally omitted — the redirect action was removed in R5.
	 * Pack the ABI generation into the top 8 bits of flags_supported so this
	 * struct stays 32 bytes (a bigger struct would overflow an old client's
	 * caller-side stack buffer under the same ioctl number — review). */
	{
		u32 flags = DRV_HWBP_FLAG_NOTIFY | DRV_HWBP_FLAG_CAPTURE_FP |
		            DRV_HWBP_FLAG_TIMING_BYPASS;
		caps.flags_supported = (flags & DRV_HWBP_CAPS_FLAGS_MASK) |
		                       ((DRV_HWBP_ABI_GENERATION & DRV_HWBP_ABI_GEN_MASK)
		                        << DRV_HWBP_ABI_GEN_SHIFT);
	}
	caps.fp_ready = hwbp_fp_ready ? 1u : 0u;
	if (copy_to_user(arg, &caps, sizeof(caps)) != 0)
		return -EFAULT;
	return 0;
}

/* Shared lookup used by every "set_*" ioctl. Enforces fd ownership. */
static struct hwbp_tracker *hwbp_lookup_by_pidaddr(s32 pid, u64 addr, struct file *owner,
                                                   struct pid **out_pid_ref) {
	struct pid *pid_ref;
	struct hwbp_tracker *tracker;

	if (hwbp_get_pid_ref((u64)pid, &pid_ref) != 0) {
		*out_pid_ref = NULL;
		return NULL;
	}
	addr = (u64)untagged_addr((unsigned long)addr);
	tracker = hwbp_lookup_locked(pid_ref, addr, owner);
	*out_pid_ref = pid_ref;
	return tracker;
}

static long hwbp_set_sample(void __user *arg, struct file *owner) {
	struct drv_hwbp_sample_req req;
	struct hwbp_tracker *tracker;
	struct pid *pid_ref;
	long rc;

	if (copy_from_user(&req, arg, sizeof(req)) != 0)
		return -EFAULT;
	if (req.pid <= 0)
		return -EINVAL;
	mutex_lock(&hwbp_mutex);
	tracker = hwbp_lookup_by_pidaddr(req.pid, req.addr, owner, &pid_ref);
	if (!tracker) { rc = -ENOENT; goto out; }
	WRITE_ONCE(tracker->sample_every, req.every);
	WRITE_ONCE(tracker->sample_counter, 0);
	rc = 0;
out:
	mutex_unlock(&hwbp_mutex);
	if (pid_ref)
		put_pid(pid_ref);
	return rc;
}

static long hwbp_set_condition(void __user *arg, struct file *owner) {
	struct drv_hwbp_condition_req req;
	struct hwbp_tracker *tracker;
	struct pid *pid_ref;
	long rc;

	if (copy_from_user(&req, arg, sizeof(req)) != 0)
		return -EFAULT;
	if (req.pid <= 0)
		return -EINVAL;
	if (req.cond_reg > 30u)
		return -EINVAL;
	if (req.cond_op > DRV_HWBP_COND_GE)
		return -EINVAL;
	mutex_lock(&hwbp_mutex);
	tracker = hwbp_lookup_by_pidaddr(req.pid, req.addr, owner, &pid_ref);
	if (!tracker) { rc = -ENOENT; goto out; }
	{
		unsigned long flags;
		/* Publish the whole (reg, op, value, active) tuple under the same lock
		 * the handler snapshots — otherwise a callback landing mid-update can
		 * mix fields from two rule versions (bug #10). */
		spin_lock_irqsave(&tracker->override_lock, flags);
		tracker->cond_reg = req.cond_reg;
		tracker->cond_op = req.cond_op;
		tracker->cond_value = req.cond_value;
		tracker->has_condition = (req.cond_op == DRV_HWBP_COND_NONE) ? 0u : 1u;
		spin_unlock_irqrestore(&tracker->override_lock, flags);
	}
	rc = 0;
out:
	mutex_unlock(&hwbp_mutex);
	if (pid_ref)
		put_pid(pid_ref);
	return rc;
}

static long hwbp_set_bypass_pid(void __user *arg, struct file *owner) {
	struct drv_hwbp_bypass_req req;
	struct hwbp_tracker *tracker;
	struct pid *pid_ref;
	long rc;

	if (copy_from_user(&req, arg, sizeof(req)) != 0)
		return -EFAULT;
	if (req.pid <= 0)
		return -EINVAL;
	mutex_lock(&hwbp_mutex);
	tracker = hwbp_lookup_by_pidaddr(req.pid, req.addr, owner, &pid_ref);
	if (!tracker) { rc = -ENOENT; goto out; }
	WRITE_ONCE(tracker->bypass_pid, req.bypass_pid);
	rc = 0;
out:
	mutex_unlock(&hwbp_mutex);
	if (pid_ref)
		put_pid(pid_ref);
	return rc;
}

static long hwbp_set_notify(void __user *arg, struct file *owner) {
	struct drv_hwbp_notify_req req;
	struct hwbp_tracker *tracker;
	struct pid *pid_ref;
	struct pid *new_notify = NULL;
	struct pid *old_notify;
	long rc;

	if (copy_from_user(&req, arg, sizeof(req)) != 0)
		return -EFAULT;
	if (req.pid <= 0)
		return -EINVAL;
	if (req.notify_pid > 0) {
		new_notify = find_get_pid(req.notify_pid);
		if (!new_notify)
			return -ESRCH;
	}
	mutex_lock(&hwbp_mutex);
	tracker = hwbp_lookup_by_pidaddr(req.pid, req.addr, owner, &pid_ref);
	if (!tracker) { rc = -ENOENT; goto out; }
	{
		unsigned long flags;
		/* Swap under notify_lock so the handler-side snapshot never sees a
		 * partially updated slot; put_pid is deferred outside the lock so we
		 * never call it in atomic context. Fixes bug #3. */
		raw_spin_lock_irqsave(&tracker->notify_lock, flags);
		old_notify = tracker->notify_pid_ref;
		tracker->notify_pid_ref = new_notify;
		tracker->notify_signal = req.signal_no;
		if (tracker->notify_pid_ref)
			tracker->flags |= DRV_HWBP_FLAG_NOTIFY;
		else
			tracker->flags &= ~DRV_HWBP_FLAG_NOTIFY;
		WRITE_ONCE(tracker->notify_in_flight, 0u);
		raw_spin_unlock_irqrestore(&tracker->notify_lock, flags);
	}
	new_notify = old_notify; /* freed below */
	rc = 0;
out:
	mutex_unlock(&hwbp_mutex);
	if (pid_ref)
		put_pid(pid_ref);
	if (new_notify)
		put_pid(new_notify);
	return rc;
}

static long hwbp_translate_bait(void __user *arg) {
	struct drv_hwbp_bait_req req;
	struct task_struct *task;
	struct mm_struct *mm;
	struct pid *pid_ref;
	long rc;

	if (copy_from_user(&req, arg, sizeof(req)) != 0)
		return -EFAULT;
	if (req.pid <= 0)
		return -EINVAL;
	rc = hwbp_get_pid_ref((u64)req.pid, &pid_ref);
	if (rc)
		return rc;
	rc = hwbp_get_task_mm(pid_ref, &task, &mm);
	if (rc) { put_pid(pid_ref); return rc; }
	req.real_addr = translate_bait(mm, (unsigned long)req.addr);
	mmput(mm);
	put_task_struct(task);
	put_pid(pid_ref);
	if (copy_to_user(arg, &req, sizeof(req)) != 0)
		return -EFAULT;
	return 0;
}

/* BAIT_GUARD (E.HWBP.6): redirect @addr into the largest contiguous VMA cluster sharing @addr's file basename. */
static bool basename_eq(const struct file *f, const char *want, char *scratch, size_t scratch_len) {
	char *p;
	if (!f) return false;
	p = d_path(&f->f_path, scratch, (int)scratch_len);
	if (IS_ERR(p)) return false;
	{
		char *slash = strrchr(p, '/');
		const char *base = slash ? slash + 1 : p;
		return strcmp(base, want) == 0;
	}
}

static unsigned long translate_bait(struct mm_struct *mm, unsigned long addr) {
	struct vm_area_struct *vma;
	unsigned long src_start = 0, src_end = 0;
	unsigned long best_start = 0, best_end = 0;
	unsigned long cur_start = 0, cur_end = 0;
	unsigned long new_addr;
	char *pathbuf;
	char *namebuf;
	char *slash;
	const char *base;
	char *p;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	VMA_ITERATOR(vmi, mm, 0);
#endif

	if (!mm || !addr)
		return addr;

	pathbuf = kmalloc(PATH_MAX, GFP_KERNEL);
	namebuf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!pathbuf || !namebuf)
		goto out_free;

	mmap_read_lock(mm);
	vma = find_vma(mm, addr);
	if (!vma || addr < vma->vm_start || !vma->vm_file) {
		mmap_read_unlock(mm);
		goto out_free;
	}
	p = d_path(&vma->vm_file->f_path, pathbuf, PATH_MAX);
	if (IS_ERR(p)) {
		mmap_read_unlock(mm);
		goto out_free;
	}
	slash = strrchr(p, '/');
	base = slash ? slash + 1 : p;
	strscpy(namebuf, base, PATH_MAX);
	src_start = vma->vm_start;
	src_end = vma->vm_end;

	/* Maple-tree iteration on 6.1+; vm_next linked list on 5.10..6.0. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	for_each_vma(vmi, vma) {
#else
	for (vma = mm->mmap; vma; vma = vma->vm_next) {
#endif
		if (!basename_eq(vma->vm_file, namebuf, pathbuf, PATH_MAX))
			continue;
		if (cur_end == 0 || vma->vm_start > cur_end + (12u * 1024u)) {
			if (cur_end - cur_start > best_end - best_start) {
				best_start = cur_start;
				best_end = cur_end;
			}
			cur_start = vma->vm_start;
		}
		cur_end = vma->vm_end;
	}
	if (cur_end - cur_start > best_end - best_start) {
		best_start = cur_start;
		best_end = cur_end;
	}
	mmap_read_unlock(mm);

	if (best_start && best_start != src_start) {
		new_addr = best_start + (addr - src_start);
		if (new_addr < best_end) {
			kfree(pathbuf);
			kfree(namebuf);
			return new_addr;
		}
	}
out_free:
	kfree(pathbuf);
	kfree(namebuf);
	return addr;
}

int hwbp_init(void) {
	u64 dfr0;
	if (hwbp_initialized)
		return hwbp_ready ? 0 : -EOPNOTSUPP;
	hwbp_initialized = true;
	BUILD_BUG_ON(sizeof(struct drv_hwbp_reg_override) != 16);
	BUILD_BUG_ON(sizeof(struct drv_hwbp_install_req) != 192);
	BUILD_BUG_ON(sizeof(struct drv_hwbp_hit) != 800);
	BUILD_BUG_ON(sizeof(struct drv_hwbp_caps) != 32);
	BUILD_BUG_ON(sizeof(struct drv_hwbp_sample_req) != 24);
	BUILD_BUG_ON(sizeof(struct drv_hwbp_condition_req) != 32);
	BUILD_BUG_ON(sizeof(struct drv_hwbp_bypass_req) != 24);
	BUILD_BUG_ON(sizeof(struct drv_hwbp_notify_req) != 24);
	BUILD_BUG_ON(sizeof(struct drv_hwbp_bait_req) != 24);

	dfr0 = read_sysreg_s(SYS_ID_AA64DFR0_EL1);
	hwbp_num_brps = (u32)(((dfr0 >> 12) & 0xFu) + 1u);
	hwbp_num_wrps = (u32)(((dfr0 >> 20) & 0xFu) + 1u);
	LOGI("hwbp: hardware caps brps=%u wrps=%u\n", hwbp_num_brps, hwbp_num_wrps);
	drv_register_user_hw_bp_ptr = (drv_register_user_hw_bp_fn_t)kallsym_lookup("register_user_hw_breakpoint");
	drv_unregister_hw_bp_ptr = (drv_unregister_hw_bp_fn_t)kallsym_lookup("unregister_hw_breakpoint");
	drv_modify_user_hw_bp_ptr = (drv_modify_user_hw_bp_fn_t)kallsym_lookup("modify_user_hw_breakpoint");
	drv_access_remote_vm_ptr = (drv_access_remote_vm_fn_t)kallsym_lookup("access_remote_vm");
	if (!drv_register_user_hw_bp_ptr || !drv_unregister_hw_bp_ptr || !drv_modify_user_hw_bp_ptr || !drv_access_remote_vm_ptr) {
		LOGN("hwbp: unavailable on this kernel\n");
		return -EOPNOTSUPP;
	}
	drv_fpsimd_preserve_current_state_ptr = (drv_fpsimd_preserve_current_state_fn_t)kallsym_lookup("fpsimd_preserve_current_state");
	drv_fpsimd_update_current_state_ptr = (drv_fpsimd_update_current_state_fn_t)kallsym_lookup("fpsimd_update_current_state");
	hwbp_fp_ready = system_supports_fpsimd() && drv_fpsimd_preserve_current_state_ptr && drv_fpsimd_update_current_state_ptr;
	hwbp_ready = true;
	return 0;
}

long do_hwbp_cmd(unsigned int cmd, void __user *arg, struct file *owner) {
	if (!hwbp_ready)
		return -EOPNOTSUPP;
	switch (cmd) {
	case DRV_CMD_HWBP_INSTALL:       return hwbp_install(arg, owner);
	case DRV_CMD_HWBP_SET_OVERRIDE:  return hwbp_set_override(arg, owner);
	case DRV_CMD_HWBP_REMOVE:        return hwbp_remove(arg, owner);
	case DRV_CMD_HWBP_GET_HITS_LEGACY:
		/* Wire-incompatible with the 800-byte hit record — refuse loudly
		 * so an old client can't silently misinterpret the payload (R4).
		 * The new command lives at 0x63 in the ext range (N1). */
		return -EPROTO;
	case DRV_CMD_HWBP_CLEAR_ALL:     return hwbp_clear_all(owner);
	case DRV_CMD_HWBP_GET_CAPS:      return hwbp_get_caps(arg);
	case DRV_CMD_HWBP_SET_SAMPLE:    return hwbp_set_sample(arg, owner);
	case DRV_CMD_HWBP_SET_CONDITION: return hwbp_set_condition(arg, owner);
	default:                         return -ENOTTY;
	}
}

long do_hwbp_ext_cmd(unsigned int cmd, void __user *arg, struct file *owner) {
	if (!hwbp_ready)
		return -EOPNOTSUPP;
	switch (cmd) {
	case DRV_CMD_HWBP_SET_BYPASS_PID: return hwbp_set_bypass_pid(arg, owner);
	case DRV_CMD_HWBP_SET_NOTIFY:     return hwbp_set_notify(arg, owner);
	case DRV_CMD_HWBP_TRANSLATE_BAIT: return hwbp_translate_bait(arg);
	case DRV_CMD_HWBP_GET_HITS:       return hwbp_get_hits(arg, owner);
	default:                          return -ENOTTY;
	}
}
