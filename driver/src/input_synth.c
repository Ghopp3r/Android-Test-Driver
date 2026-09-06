// SPDX-License-Identifier: GPL-2.0-only
// synthetic multitouch event injection
#include <linux/atomic.h>
#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/types.h>

#include <driver/types.h>
#include <driver/uapi.h>

#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
#include <linux/cfi.h>
#endif
#ifndef __nocfi
#define __nocfi
#endif

#include "input_synth.h"
#include "kallsym.h"
#include "log.h"

/* Gate the exactly-once memset; drv.init_fingers_done is the post-init flag. */
static atomic_t init_fingers_initialized = ATOMIC_INIT(0);

/* Serialise install_input_hooks so concurrent touch ioctls do not race on pool + kprobes. */
static DEFINE_MUTEX(install_lock);

/* input_handle_event() is not in __ksymtab on >= 6.6 — resolve via kallsyms at init. */
typedef void(*input_handle_event_fn_t)(struct input_dev *dev, unsigned int type, unsigned int code, int value);
static input_handle_event_fn_t input_handle_event_ptr;

static noinline __nocfi void drv_call_input_handle_event(input_handle_event_fn_t fn, struct input_dev *dev, unsigned int type, unsigned int code, int value) {
	fn(dev, type, code, value);
}

/* Caller must hold drv.pool->lock. */
static inline void pool_push(u32 type, u32 code, s32 value) {
	struct evpool *p = drv.pool;
	u64 idx;

	if (!p)
		return;

	idx = p->count;
	if (idx > DRV_EVPOOL_CAPACITY - 1)
		return;

	p->entries[idx].type = type;
	p->entries[idx].code = code;
	p->entries[idx].value = value;
	/* Publish payload before count bump; explicit for a barrier-only reader if the lock is ever dropped. */
	smp_wmb();
	p->count = idx + 1;
}

void handle_cache_events(struct input_dev *idev) {
	struct evpool *p = drv.pool;
	unsigned long flags_pool, flags_dev;
	u64 i;

	if (!p || !idev || !input_handle_event_ptr)
		return;

	spin_lock_irqsave(&p->lock, flags_pool);

	if (p->count == 0) {
		spin_unlock_irqrestore(&p->lock, flags_pool);
		return;
	}

	/* Pair with smp_wmb in pool_push; explicit even though arm64 spin_lock_irqsave is acquire. */
	smp_rmb();

	/* Hold dev->event_lock around input_handle_event — kprobed input_event would re-enter our probe. */
	spin_lock_irqsave(&idev->event_lock, flags_dev);

	if (p->count == 0)
		goto unlock_dev;

	for (i = 0; i < p->count; i++) {
		struct drv_input_event *ev;

		if (i == DRV_EVPOOL_CAPACITY + 1)
			BUG(); /* mirrors BRK #0x5512 */

		ev = &p->entries[i];

		if (ev->type > EV_MAX)
			continue;

		if (!test_bit(ev->type, idev->evbit))
			continue;

		/* kallsyms-resolved fp — call via __nocfi wrapper so kCFI does not trap. */
		drv_call_input_handle_event(input_handle_event_ptr, idev, ev->type, ev->code, ev->value);
	}

	p->count = 0;

unlock_dev:
	spin_unlock_irqrestore(&idev->event_lock, flags_dev);
	spin_unlock_irqrestore(&p->lock, flags_pool);
}

int input_handle_event_handler_pre(struct kprobe *p, struct pt_regs *regs) {
	/* input_event(dev,type,code,value): x0/x1 = dev/type; kCFI adds no implicit arg. */
	struct input_dev *idev = (struct input_dev *)regs->regs[0];
	unsigned int type = (unsigned int)regs->regs[1];

	(void)p;

	if (!idev || type != EV_SYN)
		return 0;

	handle_cache_events(idev);
	return 0;
}

int input_handle_event_handler2_pre(struct kprobe *p, struct pt_regs *regs) {
	/* input_inject_event(handle,type,code,value): x0 = handle, x1 = type. */
	struct input_handle *handle = (struct input_handle *)regs->regs[0];
	unsigned int type = (unsigned int)regs->regs[1];

	(void)p;

	if (!handle || type != EV_SYN)
		return 0;

	handle_cache_events(handle->dev);
	return 0;
}

/* Caller must hold drv.pool->lock. */
int input_mt_report_slot_state_with_id_cache(int id) {
	pool_push(EV_ABS, ABS_MT_TRACKING_ID, id);
	pool_push(EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);
	return id;
}

static void input_synth_init_fingers(void) {
	if (atomic_cmpxchg(&init_fingers_initialized, 0, 1) != 0)
		return;

	memset(drv.slots, 0, sizeof(drv.slots));
	drv.persistent_current_slot = 0;
	drv.init_fingers_done = 1;
}

/* DEVIATION from binary: dedupe against drv.persistent_current_slot; evdev caches the active slot so redundant SLOT events are safe to drop. */
static void emit_slot_select_locked(int slot) {
	if (drv.persistent_current_slot == slot)
		return;

	pool_push(EV_ABS, ABS_MT_SLOT, slot);
	drv.persistent_current_slot = slot;
}

void touch_down(int slot, int x, int y, int pressure) {
	unsigned long flags_slot, flags_pool;

	if (slot < 0 || slot >= DRV_MT_NUM_SLOTS)
		return;

	if (!drv.pool)
		return;

	input_synth_init_fingers();

	spin_lock_irqsave(&drv.slot_lock, flags_slot);
	spin_lock_irqsave(&drv.pool->lock, flags_pool);

	emit_slot_select_locked(slot);

	/* Binary 0x12D uses raw slot index (0..9) as tracking id; slot+0x29A is legacy 0x136 only. */
	drv.slots[slot].id = slot;
	drv.slots[slot].x = x;
	drv.slots[slot].y = y;
	drv.slots[slot].pressure = pressure;
	drv.slots[slot].active = 1;

	input_mt_report_slot_state_with_id_cache(drv.slots[slot].id);
	pool_push(EV_ABS, ABS_MT_POSITION_X, x);
	pool_push(EV_ABS, ABS_MT_POSITION_Y, y);
	/* Binary emits ABS_MT_ORIENTATION between POSITION_Y and TOUCH_MAJOR carrying pressure. */
	pool_push(EV_ABS, ABS_MT_ORIENTATION, pressure);
	pool_push(EV_ABS, ABS_MT_TOUCH_MAJOR, pressure);
	pool_push(EV_ABS, ABS_MT_PRESSURE, pressure);
	/* Binary 0x12D pushes ONLY ABS_MT events; no BTN_TOUCH / BTN_TOOL_FINGER. */

	spin_unlock_irqrestore(&drv.pool->lock, flags_pool);
	spin_unlock_irqrestore(&drv.slot_lock, flags_slot);
}

void touch_move(int slot, int x, int y) {
	unsigned long flags_slot, flags_pool;

	if (slot < 0 || slot >= DRV_MT_NUM_SLOTS)
		return;

	if (!drv.pool)
		return;

	input_synth_init_fingers();

	spin_lock_irqsave(&drv.slot_lock, flags_slot);
	spin_lock_irqsave(&drv.pool->lock, flags_pool);

	emit_slot_select_locked(slot);

	drv.slots[slot].x = x;
	drv.slots[slot].y = y;

	pool_push(EV_ABS, ABS_MT_POSITION_X, x);
	pool_push(EV_ABS, ABS_MT_POSITION_Y, y);

	spin_unlock_irqrestore(&drv.pool->lock, flags_pool);
	spin_unlock_irqrestore(&drv.slot_lock, flags_slot);
}

void touch_up(int slot) {
	unsigned long flags_slot, flags_pool;

	if (slot < 0 || slot >= DRV_MT_NUM_SLOTS)
		return;

	if (!drv.pool)
		return;

	input_synth_init_fingers();

	spin_lock_irqsave(&drv.slot_lock, flags_slot);
	spin_lock_irqsave(&drv.pool->lock, flags_pool);

	emit_slot_select_locked(slot);

	/* Binary 0x12E pushes only ABS_MT_TRACKING_ID == -1; BTN_TOUCH release comes from real driver. */
	pool_push(EV_ABS, ABS_MT_TRACKING_ID, -1);

	drv.slots[slot].id = -1;
	drv.slots[slot].active = 0;

	spin_unlock_irqrestore(&drv.pool->lock, flags_pool);
	spin_unlock_irqrestore(&drv.slot_lock, flags_slot);
}

int install_input_hooks(void) {
	int ret = 0;
	bool input_event_registered_now = false;

	/* Serialise the lazy-init: two CPUs racing here would double-register the kprobe. */
	/* Safe in process context; no kprobe pre-handler reaches this path. */
	mutex_lock(&install_lock);

	if (!input_handle_event_ptr) {
		/* input_handle_event has no __ksymtab entry on >= 6.6; resolve via kallsyms. */
		input_handle_event_ptr = (input_handle_event_fn_t)kallsym_lookup("input_handle_event");
		if (!input_handle_event_ptr) {
			LOGE("input_synth: failed to resolve input_handle_event via kallsyms\n");
			ret = -ENOENT;
			goto out_unlock;
		}
	}

	if (!drv.pool) {
		struct evpool *p;

		/* Mirror binary's retry hint (gfp 0xCC0) so allocator behaviour matches under pressure. */
		p = kvzalloc(sizeof(*p), GFP_KERNEL | __GFP_RETRY_MAYFAIL);
		if (!p) {
			LOGE("input_synth: failed to alloc event pool\n");
			ret = -ENOMEM;
			goto out_unlock;
		}

		spin_lock_init(&p->lock);
		p->count = 0;

		/* Prime slot_lock lockdep BEFORE drv.pool is published so any reader sees a usable lock. */
		spin_lock_init(&drv.slot_lock);
		input_synth_init_fingers();

		/* Release-publish drv.pool after everything is set up; install_lock makes this documentary. */
		smp_store_release(&drv.pool, p);
	}

	if (!drv.kp_input_event_armed) {
		drv.kp_input_event.symbol_name = "input_event";
		drv.kp_input_event.pre_handler = input_handle_event_handler_pre;
		ret = register_kprobe(&drv.kp_input_event);
		if (ret) {
			LOGE("Failed to register kprobe kp_input_event: %d\n", ret);
			/* register_kprobe may have resolved symbol_name -> addr; clear so a retry does not present both. */
			drv.kp_input_event.addr = NULL;
			goto out_unlock;
		}
		drv.kp_input_event_armed = 1;
		input_event_registered_now = true;
	}

	if (!drv.kp_input_inject_armed) {
		drv.kp_input_inject_event.symbol_name = "input_inject_event";
		drv.kp_input_inject_event.pre_handler = input_handle_event_handler2_pre;
		ret = register_kprobe(&drv.kp_input_inject_event);
		if (ret) {
			LOGE("Failed to register kprobe kp_input_inject_event: %d\n", ret);
			drv.kp_input_inject_event.addr = NULL;
			if (input_event_registered_now) {
				unregister_kprobe(&drv.kp_input_event);
				drv.kp_input_event_armed = 0;
				drv.kp_input_event.addr = NULL;
			}
			goto out_unlock;
		}
		drv.kp_input_inject_armed = 1;
	}

out_unlock:
	mutex_unlock(&install_lock);
	return ret;
}
