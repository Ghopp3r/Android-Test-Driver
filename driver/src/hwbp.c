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

	/* Gate fields (mutated under override_lock; read lock-free in the handler). */
	u32 sample_every;            /* 0 = every hit */
	u32 sample_counter;
	u32 has_condition;
	u32 cond_op;                 /* DRV_HWBP_COND_* */
	u32 cond_reg;                /* 0..30 (X-reg index) */
	u32 _cond_pad;
	u64 cond_value;
	s32 bypass_pid;              /* one-shot: cleared to 0 after consumed */

	/* Async signal delivery (E.HWBP.1). */
	struct pid *notify_pid_ref;
	u32 notify_signal;           /* 0 = SIGRTMIN+1 (34) */
	u32 notify_seq;              /* debouncer — increment in handler, drain in get_hits */
	u32 notify_in_flight;        /* single outstanding worker */
	u32 tracker_id;              /* stable id for si_int payload */
};

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

/* Forward declarations for helpers used inside the handler. */
static unsigned long translate_bait(struct mm_struct *mm, unsigned long addr);
static void hwbp_notify_worker(struct work_struct *w);

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

	/* FPSIMD save runs in this task's context — the perf overflow handler is
	 * called from the exception path with current == the target task. Do it
	 * BEFORE acquiring the ring spinlock: the helper may sleep on 5.10 (it
	 * flushes the SVE state), and we cannot hold a raw spinlock across that. */
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

/* Evaluate the optional condition guard: returns true if the handler should
 * proceed (condition matched OR no condition set). cond_reg indexes X0..X30. */
static bool hwbp_condition_matches(const struct hwbp_tracker *tracker, const struct pt_regs *regs) {
	u64 v;
	if (!READ_ONCE(tracker->has_condition))
		return true;
	if (tracker->cond_reg > 30u)
		return true;
	v = regs->regs[tracker->cond_reg];
	switch (tracker->cond_op) {
	case DRV_HWBP_COND_EQ: return v == tracker->cond_value;
	case DRV_HWBP_COND_NE: return v != tracker->cond_value;
	case DRV_HWBP_COND_LT: return (s64)v <  (s64)tracker->cond_value;
	case DRV_HWBP_COND_LE: return (s64)v <= (s64)tracker->cond_value;
	case DRV_HWBP_COND_GT: return (s64)v >  (s64)tracker->cond_value;
	case DRV_HWBP_COND_GE: return (s64)v >= (s64)tracker->cond_value;
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

static void hwbp_maybe_notify(struct hwbp_tracker *tracker) {
	struct hwbp_notify_work *nw;
	struct pid *pid_ref;
	int signal_no;

	if (!(tracker->flags & DRV_HWBP_FLAG_NOTIFY))
		return;
	if (cmpxchg(&tracker->notify_in_flight, 0u, 1u) != 0u)
		return;
	pid_ref = READ_ONCE(tracker->notify_pid_ref);
	if (!pid_ref) {
		WRITE_ONCE(tracker->notify_in_flight, 0u);
		return;
	}
	nw = kmalloc(sizeof(*nw), GFP_ATOMIC);
	if (!nw) {
		WRITE_ONCE(tracker->notify_in_flight, 0u);
		return;
	}
	nw->pid = get_pid(pid_ref);
	signal_no = READ_ONCE(tracker->notify_signal);
	nw->signal_no = signal_no ? signal_no : (SIGRTMIN + 1);
	nw->bp_id = (int)tracker->tracker_id;
	INIT_WORK(&nw->work, hwbp_notify_worker);
	queue_work(system_wq, &nw->work);
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

	/* pass_through toggle path: the second hit is our own re-arm — rearm to
	 * origin and skip all gates. Applies only to execute breakpoints. */
	if (tracker->pass_through && READ_ONCE(tracker->toggle) == HWBP_TOGGLE_NEXT) {
		rc = hwbp_set_breakpoint_address(bp, tracker->addr);
		if (rc) {
			LOGW_RL("hwbp: rearm rc=%d\n", rc);
			return;
		}
		WRITE_ONCE(tracker->toggle, HWBP_TOGGLE_ORIGIN);
		return;
	}

	/* Rate-limited handler diagnostics — enough to see 3-4 hits then quiet. */
	{
		static atomic_t dbg_seen = ATOMIC_INIT(0);
		int seen = atomic_read(&dbg_seen);
		if (seen < 12) {
			atomic_inc(&dbg_seen);
			LOGI("hwbp: hit tracker=%px cur.pid=%d cur.tgid=%d bypass=%d sample_every=%u sample_ctr=%u flags=%x\n",
			     tracker, current->pid, current->tgid,
			     tracker->bypass_pid, tracker->sample_every,
			     tracker->sample_counter, tracker->flags);
		}
	}

	/* Bypass gate (one-shot): match on the target process, not just the
	 * thread — userspace uses getpid() (TGID) but current->pid is TID. Use
	 * current->tgid so the gate works for multi-threaded clients too. */
	{
		s32 by = READ_ONCE(tracker->bypass_pid);
		if (by && by == (s32)current->tgid) {
			cmpxchg(&tracker->bypass_pid, by, 0);
			return;
		}
	}

	/* Sample gate: fire only when sample_counter % sample_every == 0. */
	{
		u32 every = READ_ONCE(tracker->sample_every);
		if (every) {
			u32 c = ++tracker->sample_counter;
			if (c % every != 0)
				return;
		}
	}

	/* Condition gate: skip if optional {reg, op, value} does not match. */
	if (!hwbp_condition_matches(tracker, regs))
		return;

	/* TIMING_BYPASS: no ring push, no signal — reduces observable overhead to
	 * the perf overflow path alone. Register overrides still applied. */
	timing_bypass = !!(tracker->flags & DRV_HWBP_FLAG_TIMING_BYPASS);

	count = hwbp_snapshot_overrides(tracker, overrides);
	if (!timing_bypass) {
		hwbp_ring_push(tracker, regs);
		hwbp_maybe_notify(tracker);
	}
	if (hwbp_has_fp_override(overrides, count))
		hwbp_apply_fp(overrides, count);
	hwbp_apply_gp(regs, overrides, count);
	if (!tracker->pass_through) {
		if (!hwbp_overrides_have_pc(overrides, count))
			regs->pc = regs->regs[30];
		return;
	}

	rc = hwbp_set_breakpoint_address(bp, tracker->addr + DRV_HWBP_LEN_EXECUTE);
	if (rc) {
		LOGW_RL("hwbp: advance rc=%d\n", rc);
		return;
	}
	WRITE_ONCE(tracker->toggle, HWBP_TOGGLE_NEXT);
}
NOKPROBE_SYMBOL(hwbp_handler);

static void hwbp_notify_worker(struct work_struct *w) {
	struct hwbp_notify_work *nw = container_of(w, struct hwbp_notify_work, work);
	struct task_struct *task;
	struct kernel_siginfo info;

	if (!nw->pid)
		goto out;
	task = get_pid_task(nw->pid, PIDTYPE_TGID);
	if (!task)
		task = get_pid_task(nw->pid, PIDTYPE_PID);
	if (!task)
		goto out_put_pid;

	memset(&info, 0, sizeof(info));
	info.si_signo = nw->signal_no;
	info.si_code = SI_QUEUE;
	info.si_int = nw->bp_id;
	send_sig_info(nw->signal_no, &info, task);
	put_task_struct(task);

	/* Best-effort in_flight clear (tracker may already be gone). */
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

out_put_pid:
	put_pid(nw->pid);
out:
	kfree(nw);
}

static struct hwbp_tracker *hwbp_lookup_locked(struct pid *pid_ref, u64 addr) {
	struct hwbp_tracker *tracker;

	list_for_each_entry(tracker, &hwbp_trackers, node) {
		if (tracker->pid_ref == pid_ref && tracker->addr == addr)
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

	/* Execute breakpoints trap on the aarch64 4-byte instruction boundary.
	 * Watchpoints trap on any bp_len-aligned span (arm64 supports 1/2/4/8). */
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

	/* Accept every combination the ARMv8 debug spec allows: X requires len=4
	 * (single instruction), watchpoints (R/W/RW) allow 1/2/4/8. */
	switch (req->bp_type) {
	case DRV_HWBP_TYPE_R:
	case DRV_HWBP_TYPE_W:
	case DRV_HWBP_TYPE_RW:
		if (req->bp_len != DRV_HWBP_LEN_1 && req->bp_len != DRV_HWBP_LEN_2 &&
		    req->bp_len != DRV_HWBP_LEN_4 && req->bp_len != DRV_HWBP_LEN_8)
			return -EOPNOTSUPP;
		/* pass_through has no fall-through semantics for watchpoints — the
		 * data access does not advance PC; toggling the address would just
		 * re-fire on the next load/store to the same location. */
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

	/* BAIT_GUARD: translate the requested addr into the LARGEST contiguous
	 * VMA cluster with the same file basename BEFORE we validate the address
	 * — an AC bait mmap yields a small non-primary cluster; ours resolves
	 * anywhere in that same file. If translation returned the input as-is,
	 * the address was already in the largest cluster (or had no file). */
	if (req.flags & DRV_HWBP_FLAG_BAIT_GUARD) {
		unsigned long real = translate_bait(mm, (unsigned long)req.addr);
		if (real && real != (unsigned long)req.addr) {
			LOGI("hwbp: bait_guard %llx -> %lx\n",
			     (unsigned long long)req.addr, real);
			req.addr = real;
		}
	}

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
	existing = hwbp_lookup_locked(pid_ref, req.addr);
	if (existing && existing->mm == mm) {
		if (existing->bp_type != req.bp_type || existing->bp_len != req.bp_len || existing->pass_through != req.pass_through) {
			rc = -EBUSY;
		} else {
			/* Update mutable install-time state — flags mask + overrides. */
			existing->flags = req.flags;
			hwbp_set_overrides(existing, &req);
			rc = 0;
		}
		mutex_unlock(&hwbp_mutex);
		goto out_put_task_mm;
	}
	if (existing) {
		list_del(&existing->node);
		hwbp_unregister_and_free(existing);
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

static long hwbp_set_override(void __user *arg) {
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
	tracker = hwbp_lookup_locked(pid_ref, req.addr);
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

static long hwbp_remove(void __user *arg) {
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
	tracker = hwbp_lookup_locked(pid_ref, req.addr);
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

static long hwbp_get_hits(void __user *arg) {
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
	tracker = hwbp_lookup_locked(pid_ref, req.addr);
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

static long hwbp_clear_all(void) {
	struct hwbp_tracker *tracker;
	struct hwbp_tracker *next;

	mutex_lock(&hwbp_mutex);
	list_for_each_entry_safe(tracker, next, &hwbp_trackers, node) {
		list_del(&tracker->node);
		hwbp_unregister_and_free(tracker);
	}
	mutex_unlock(&hwbp_mutex);
	return 0;
}

/* fd-scoped cleanup (A.2): remove only the trackers whose owning fd is @f.
 * Called from the file_operations .release path in comm.c so a client crash
 * or explicit close reliably reclaims its own HWBP slots without touching
 * unrelated clients' trackers. */
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
	caps.flags_supported = DRV_HWBP_FLAG_BAIT_GUARD | DRV_HWBP_FLAG_NOTIFY |
			       DRV_HWBP_FLAG_CAPTURE_FP | DRV_HWBP_FLAG_TIMING_BYPASS;
	caps.fp_ready = hwbp_fp_ready ? 1u : 0u;
	if (copy_to_user(arg, &caps, sizeof(caps)) != 0)
		return -EFAULT;
	return 0;
}

/* Shared lookup + apply used by every "set_*" ioctl below. */
static struct hwbp_tracker *hwbp_lookup_by_pidaddr(s32 pid, u64 addr, struct pid **out_pid_ref) {
	struct pid *pid_ref;
	struct hwbp_tracker *tracker;

	if (hwbp_get_pid_ref((u64)pid, &pid_ref) != 0) {
		*out_pid_ref = NULL;
		return NULL;
	}
	addr = (u64)untagged_addr((unsigned long)addr);
	tracker = hwbp_lookup_locked(pid_ref, addr);
	*out_pid_ref = pid_ref;
	return tracker;
}

static long hwbp_set_sample(void __user *arg) {
	struct drv_hwbp_sample_req req;
	struct hwbp_tracker *tracker;
	struct pid *pid_ref;
	long rc;

	if (copy_from_user(&req, arg, sizeof(req)) != 0)
		return -EFAULT;
	if (req.pid <= 0)
		return -EINVAL;
	mutex_lock(&hwbp_mutex);
	tracker = hwbp_lookup_by_pidaddr(req.pid, req.addr, &pid_ref);
	if (!tracker) { rc = -ENOENT; goto out; }
	WRITE_ONCE(tracker->sample_every, req.every);
	WRITE_ONCE(tracker->sample_counter, 0);
	LOGI("hwbp: set_sample pid=%d addr=%px every=%u -> tracker=%px counter=0\n",
	     req.pid, (void *)(uintptr_t)req.addr, req.every, tracker);
	rc = 0;
out:
	mutex_unlock(&hwbp_mutex);
	if (pid_ref)
		put_pid(pid_ref);
	return rc;
}

static long hwbp_set_condition(void __user *arg) {
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
	tracker = hwbp_lookup_by_pidaddr(req.pid, req.addr, &pid_ref);
	if (!tracker) { rc = -ENOENT; goto out; }
	tracker->cond_reg = req.cond_reg;
	tracker->cond_op = req.cond_op;
	tracker->cond_value = req.cond_value;
	/* Publish has_condition last so the handler sees a consistent snapshot. */
	smp_wmb();
	WRITE_ONCE(tracker->has_condition, req.cond_op == DRV_HWBP_COND_NONE ? 0u : 1u);
	rc = 0;
out:
	mutex_unlock(&hwbp_mutex);
	if (pid_ref)
		put_pid(pid_ref);
	return rc;
}

static long hwbp_set_bypass_pid_ioctl(void __user *arg) {
	struct drv_hwbp_bypass_req req;
	struct hwbp_tracker *tracker;
	struct pid *pid_ref;
	long rc;

	if (copy_from_user(&req, arg, sizeof(req)) != 0)
		return -EFAULT;
	if (req.pid <= 0)
		return -EINVAL;
	mutex_lock(&hwbp_mutex);
	tracker = hwbp_lookup_by_pidaddr(req.pid, req.addr, &pid_ref);
	if (!tracker) { rc = -ENOENT; goto out; }
	WRITE_ONCE(tracker->bypass_pid, req.bypass_pid);
	LOGI("hwbp: set_bypass pid=%d addr=%px bypass_pid=%d -> tracker=%px\n",
	     req.pid, (void *)(uintptr_t)req.addr, req.bypass_pid, tracker);
	rc = 0;
out:
	mutex_unlock(&hwbp_mutex);
	if (pid_ref)
		put_pid(pid_ref);
	return rc;
}

static long hwbp_set_notify_ioctl(void __user *arg) {
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
	tracker = hwbp_lookup_by_pidaddr(req.pid, req.addr, &pid_ref);
	if (!tracker) { rc = -ENOENT; goto out; }
	old_notify = tracker->notify_pid_ref;
	tracker->notify_pid_ref = new_notify;
	new_notify = old_notify; /* freed below */
	tracker->notify_signal = req.signal_no;
	if (tracker->notify_pid_ref)
		tracker->flags |= DRV_HWBP_FLAG_NOTIFY;
	else
		tracker->flags &= ~DRV_HWBP_FLAG_NOTIFY;
	WRITE_ONCE(tracker->notify_in_flight, 0u);
	rc = 0;
out:
	mutex_unlock(&hwbp_mutex);
	if (pid_ref)
		put_pid(pid_ref);
	if (new_notify)
		put_pid(new_notify);
	return rc;
}

static long hwbp_translate_bait_ioctl(void __user *arg) {
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

/* BAIT_GUARD implementation (E.HWBP.6): scan the target mm's VMAs, cluster
 * contiguous ranges that share a file basename, and redirect @addr into the
 * LARGEST such cluster. Callers that pass an address in a legit-looking bait
 * mmap (typical anti-cheat setup) end up placing the HWBP on the true module
 * mapping instead. Returns @addr unchanged when no better target is found. */
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
		case DRV_CMD_HWBP_INSTALL:
			return hwbp_install(arg, owner);
		case DRV_CMD_HWBP_SET_OVERRIDE:
			return hwbp_set_override(arg);
		case DRV_CMD_HWBP_REMOVE:
			return hwbp_remove(arg);
		case DRV_CMD_HWBP_GET_HITS:
			return hwbp_get_hits(arg);
		case DRV_CMD_HWBP_CLEAR_ALL:
			return hwbp_clear_all();
		case DRV_CMD_HWBP_GET_CAPS:
			return hwbp_get_caps(arg);
		case DRV_CMD_HWBP_SET_SAMPLE:
			return hwbp_set_sample(arg);
		case DRV_CMD_HWBP_SET_CONDITION:
			return hwbp_set_condition(arg);
		default:
			return -ENOTTY;
	}
}

long do_hwbp_ext_cmd(unsigned int cmd, void __user *arg) {
	if (!hwbp_ready)
		return -EOPNOTSUPP;
	switch (cmd) {
		case DRV_CMD_HWBP_SET_BYPASS_PID:
			return hwbp_set_bypass_pid_ioctl(arg);
		case DRV_CMD_HWBP_SET_NOTIFY:
			return hwbp_set_notify_ioctl(arg);
		case DRV_CMD_HWBP_TRANSLATE_BAIT:
			return hwbp_translate_bait_ioctl(arg);
		default:
			return -ENOTTY;
	}
}
