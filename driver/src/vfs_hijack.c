// SPDX-License-Identifier: GPL-2.0-only
// /dev/input/event* read interception: kprobe __arm64_sys_read, swap f_op
// for uid 1000 evdev fds so read_proxy can rewrite the MT-B stream.

#include <linux/cred.h>
#include <linux/dcache.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/input.h>
#include <linux/kprobes.h>
#include <linux/mutex.h>
#include <linux/path.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <linux/workqueue.h>

#include <driver/types.h>
#include <driver/uapi.h>

#include "log.h"
#include "vfs_hijack.h"

/* Pin install/teardown to CPU 32 to match queue_work_on(32, ...) in the original. */
#define VFS_HIJACK_WORK_CPU 32

/* AID_SYSTEM */
#define VFS_HIJACK_TARGET_UID 1000u

#define VFS_HIJACK_PATH_PREFIX "/dev/input/event"
#define VFS_HIJACK_PATH_PREFIX_LEN 16

#define DPATH_BUF_BYTES 256

#define EVBIT_EV_ABS_MASK (1UL << EV_ABS)
#define ABSBIT_MT_POSITION_X_MASK (1UL << ABS_MT_POSITION_X)
#define ABSBIT_MT_POSITION_Y_MASK (1UL << ABS_MT_POSITION_Y)

/* struct evdev has no public header; layout stable on android15-6.6: client+0x30 -> evdev, evdev+0x20 -> input_dev. */
#define EVDEV_CLIENT_EVDEV_OFFSET 0x30u
#define EVDEV_INPUT_DEV_OFFSET 0x20u

/* sizeof (struct input_event) on the 6.6 build (8+8+2+2+4). */
#define INPUT_EVENT_SIZE 24u

#define MAX_EVENTS_PER_CALL 256
#define READ_EVENTS_BUF_BYTES (MAX_EVENTS_PER_CALL * INPUT_EVENT_SIZE)
#define FINAL_EVENTS_BUF_BYTES (READ_EVENTS_BUF_BYTES + 8)

static struct kprobe vfs_read_kp = {
	.symbol_name = "__arm64_sys_read",
	.pre_handler = sys_read_handler_pre,
};

static struct file_operations fops_proxy;

static ssize_t(*orig_read)(struct file *, char __user *, size_t, loff_t *);

static struct work_struct stop_vfs_read_work;

/* Guards stop_vfs_read_hook from double-unregistering after the queued work already tore down. */
static bool vfs_read_kp_armed;

/* Set on first f_op swap so subsequent qualifying reads skip re-install. */
static int vfs_read_hook_rc_inserted;

static struct input_dev *dev_cache;

static DEFINE_MUTEX(slot_lock);

/* 0x14-byte stride matches original: +0 sticky active, +4 tracking_id, +8 x, +0xC y, +0x10 touched-this-syscall. */
struct vfs_mt_slot {
	u8 active;
	u8 _pad1;
	u16 _pad2;
	s32 tracking_id;
	s32 x;
	s32 y;
	u8 touched_this_frame;
	u8 _pad3[3];
};
static struct vfs_mt_slot slots[DRV_MT_NUM_SLOTS];

/* Default when the real evdev stream omits TOUCH_MAJOR/PRESSURE; original emits 1 per touched slot. */
#define VFS_HIJACK_DEFAULT_PRESSURE 1

/* MT-B only sends ABS_MT_SLOT on slot change, so it must be sticky. */
static int persistent_current_slot;

static int init_fingers_done;

static int isfirstdown;

static u8 read_events[READ_EVENTS_BUF_BYTES];
static u8 final_events[FINAL_EVENTS_BUF_BYTES];
static u32 final_event_count;

/* Reach input_dev via EVDEV_*_OFFSET above (struct evdev has no public header). */
static struct input_dev *get_evdev_dev(struct file *file) {
	void *client;
	void *evdev;

	client = file->private_data;
	if (!client)
		return NULL;

	evdev = *(void **)((u8 *)client + EVDEV_CLIENT_EVDEV_OFFSET);
	if (!evdev)
		return NULL;

	return *(struct input_dev **)((u8 *)evdev + EVDEV_INPUT_DEV_OFFSET);
}

static bool file_is_mt_evdev(struct file *file) {
	struct input_dev *indev;
	unsigned long evbit0;
	unsigned long absbit0;

	indev = get_evdev_dev(file);
	if (!indev)
		return false;

	evbit0 = indev->evbit[0];
	absbit0 = indev->absbit[0];

	if (!(evbit0 & EVBIT_EV_ABS_MASK))
		return false;
	if (!(absbit0 & ABSBIT_MT_POSITION_X_MASK))
		return false;
	if (!(absbit0 & ABSBIT_MT_POSITION_Y_MASK))
		return false;

	return true;
}

static void install_fops_proxy(struct file *file) {
	const struct file_operations *real_fops;

	real_fops = file->f_op;

	/* Designated init dodges removed-in-6.5 (iterate/sendpage) and added-in-6.12 (fop_flags) slots. */
	fops_proxy = (struct file_operations) {
		.owner = real_fops->owner,
		.llseek = real_fops->llseek,
		.read = real_fops->read,
		.write = real_fops->write,
		.read_iter = real_fops->read_iter,
		.write_iter = real_fops->write_iter,
		.iopoll = real_fops->iopoll,
		.iterate_shared = real_fops->iterate_shared,
		.poll = real_fops->poll,
		.unlocked_ioctl = real_fops->unlocked_ioctl,
		.compat_ioctl = real_fops->compat_ioctl,
		.mmap = real_fops->mmap,
		.open = real_fops->open,
		.flush = real_fops->flush,
		.release = real_fops->release,
		.fsync = real_fops->fsync,
		.fasync = real_fops->fasync,
		.lock = real_fops->lock,
		.get_unmapped_area = real_fops->get_unmapped_area,
		.check_flags = real_fops->check_flags,
		.flock = real_fops->flock,
		.splice_write = real_fops->splice_write,
		.splice_read = real_fops->splice_read,
		.setlease = real_fops->setlease,
		.fallocate = real_fops->fallocate,
		.show_fdinfo = real_fops->show_fdinfo,
		.copy_file_range = real_fops->copy_file_range,
		.remap_file_range = real_fops->remap_file_range,
		.fadvise = real_fops->fadvise,
	};

	orig_read = real_fops->read;
	if (orig_read)
		fops_proxy.read = read_proxy;

	file->f_op = &fops_proxy;
}

static void vfs_hijack_init_fingers(void) {
	int i;

	if (init_fingers_done)
		return;

	for (i = 0; i < DRV_MT_NUM_SLOTS; i++) {
		slots[i].active = 0;
		slots[i]._pad1 = 0;
		slots[i]._pad2 = 0;
		slots[i].tracking_id = -1;
		slots[i].x = 0;
		slots[i].y = 0;
		slots[i].touched_this_frame = 0;
	}
	init_fingers_done = 1;
}

static void final_events_push(u16 type, u16 code, s32 value) {
	struct input_event ev;
	u8 *dst;

	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.code = code;
	ev.value = value;

	dst = final_events + (size_t)final_event_count * INPUT_EVENT_SIZE;
	memcpy(dst, &ev, sizeof(ev));
	final_event_count++;
}

static void parse_real_event(const struct input_event *ev) {
	int slot;

	if (ev->type != EV_ABS)
		return;

	if (ev->code == ABS_MT_SLOT) {
		if (ev->value >= 0 && ev->value < DRV_MT_NUM_SLOTS)
			persistent_current_slot = ev->value;
		return;
	}

	slot = persistent_current_slot;
	if (slot < 0 || slot >= DRV_MT_NUM_SLOTS)
		return;

	/* Original sets touched-this-syscall on any ABS_MT_* arrival. */
	slots[slot].touched_this_frame = 1;

	switch (ev->code) {
		case ABS_MT_TRACKING_ID:
			slots[slot].tracking_id = ev->value;
			/* Sticky active flag; only TRACKING_ID == -1 clears it (finger up). */
			if (ev->value == -1)
				slots[slot].active = 0;
			else
				slots[slot].active = 1;
			break;
		case ABS_MT_POSITION_X:
			slots[slot].x = ev->value;
			/* MT-B slot reuse may deliver POSITION without a fresh TRACKING_ID; still mark active. */
			slots[slot].active = 1;
			break;
		case ABS_MT_POSITION_Y:
			slots[slot].y = ev->value;
			slots[slot].active = 1;
			break;
		default:
			break;
	}
}

static int count_active_slots(void) {
	int i;
	int n = 0;

	for (i = 0; i < DRV_MT_NUM_SLOTS; i++)
		if (slots[i].active)
			n++;
	return n;
}

static void synthesize_events(void) {
	int i;
	int last_slot;
	int active_now;
	int want_down;
	s32 pressure;

	final_event_count = 0;
	last_slot = -1;

	/* Emit only touched-this-syscall slots (matches original CBZ skip at 0x13668). */
	for (i = 0; i < DRV_MT_NUM_SLOTS; i++) {
		if (!slots[i].touched_this_frame)
			continue;

		/* 6 EV_ABS codes per slot (SLOT only on change, so worst case 6). */
		if (final_event_count + 6 > MAX_EVENTS_PER_CALL)
			break;

		if (last_slot != i) {
			final_events_push(EV_ABS, ABS_MT_SLOT, i);
			last_slot = i;
		}

		final_events_push(EV_ABS, ABS_MT_TRACKING_ID, slots[i].tracking_id);
		final_events_push(EV_ABS, ABS_MT_POSITION_X, slots[i].x);
		final_events_push(EV_ABS, ABS_MT_POSITION_Y, slots[i].y);

		/* Fixed pressure 1 per touched slot (matches original TOUCH_MAJOR + PRESSURE emit). */
		pressure = slots[i].active ? VFS_HIJACK_DEFAULT_PRESSURE : 0;
		final_events_push(EV_ABS, ABS_MT_TOUCH_MAJOR, pressure);
		final_events_push(EV_ABS, ABS_MT_PRESSURE, pressure);
	}

	/* Clear touched-this-frame; sticky active persists until TRACKING_ID == -1. */
	for (i = 0; i < DRV_MT_NUM_SLOTS; i++)
		slots[i].touched_this_frame = 0;

	active_now = count_active_slots();
	want_down = (active_now > 0);

	if (want_down && !isfirstdown) {
		if (final_event_count + 2 <= MAX_EVENTS_PER_CALL) {
			final_events_push(EV_KEY, BTN_TOUCH, 1);
			final_events_push(EV_KEY, BTN_TOOL_FINGER, 1);
		}
		isfirstdown = 1;
	} else if (!want_down && isfirstdown) {
		if (final_event_count + 2 <= MAX_EVENTS_PER_CALL) {
			final_events_push(EV_KEY, BTN_TOUCH, 0);
			final_events_push(EV_KEY, BTN_TOOL_FINGER, 0);
		}
		isfirstdown = 0;
	}

	if (final_event_count < MAX_EVENTS_PER_CALL)
		final_events_push(EV_SYN, SYN_REPORT, 0);
}

int sys_read_handler_pre(struct kprobe *p, struct pt_regs *regs) {
	struct pt_regs *syscall_regs;
	unsigned int fd;
	struct file *file;
	struct dentry *dentry;
	const char *short_name;
	const char *full_path;
	char path_buf[DPATH_BUF_BYTES];
	const struct cred *cred;

	(void)p;

	/* __arm64_sys_read wrapper: x0 is (const struct pt_regs *), so fd is a double-deref. */
	syscall_regs = (struct pt_regs *)regs->regs[0];
	if (!syscall_regs)
		return 0;
	fd = (unsigned int)syscall_regs->regs[0];

	/* fget() returns NULL on bad fd (never ERR_PTR); IS_ERR-then-fput would fput a poison pointer. */
	file = fget(fd);
	if (!file)
		return 0;

	cred = current_cred();
	if (!cred || __kuid_val(cred->uid) != VFS_HIJACK_TARGET_UID)
		goto out_put;

	dentry = file->f_path.dentry;
	if (!dentry)
		goto out_put;
	/* qstr.name is const unsigned char *; cast silences -Wpointer-sign for strstr/strncmp. */
	short_name = (const char *)dentry->d_name.name;
	if (!short_name || !strstr(short_name, "event"))
		goto out_put;

	memset(path_buf, 0, sizeof(path_buf));
	full_path = d_path(&file->f_path, path_buf, sizeof(path_buf));
	if (IS_ERR(full_path))
		goto out_put;
	if (strncmp(full_path, VFS_HIJACK_PATH_PREFIX, VFS_HIJACK_PATH_PREFIX_LEN) != 0)
		goto out_put;

	if (!file_is_mt_evdev(file))
		goto out_put;

	if (vfs_read_hook_rc_inserted == 1) {
		queue_work_on(VFS_HIJACK_WORK_CPU, system_wq, &stop_vfs_read_work);
	} else {
		vfs_read_hook_rc_inserted = 1;
		LOGN("vfs_read_hijack: find evdev %s devname %s task %s\n", full_path, short_name, current->comm);
		install_fops_proxy(file);
	}

out_put:
	fput(file);
	return 0;
}

void do_stop_vfs_read_hook(struct work_struct *work) {
	(void)work;
	if (vfs_read_kp_armed) {
		unregister_kprobe(&vfs_read_kp);
		vfs_read_kp_armed = false;
	}
}

ssize_t read_proxy(struct file *file, char __user *buf, size_t count, loff_t *ppos) {
	ssize_t real;
	size_t i;
	size_t parse_bytes;
	size_t out_bytes;

	if (!dev_cache)
		dev_cache = get_evdev_dev(file);

	mutex_lock(&slot_lock);

	vfs_hijack_init_fingers();

	/* Typed pointer so the compiler emits the CFI typeid check (matches original's 0xE866E2F4 + BLR X8). */
	if (!orig_read) {
		mutex_unlock(&slot_lock);
		return -EIO;
	}
	real = orig_read(file, buf, count, ppos);

	/* Original falls through to synth on short/error/misaligned; SYN_REPORT terminator hides EAGAIN/EINTR/EIO. */
	parse_bytes = 0;
	if (real >= (ssize_t)INPUT_EVENT_SIZE && ((size_t)real % INPUT_EVENT_SIZE) == 0 && (size_t)real <= READ_EVENTS_BUF_BYTES) {
		parse_bytes = (size_t)real;
		memset(read_events, 0, READ_EVENTS_BUF_BYTES);
		if (copy_from_user(read_events, buf, parse_bytes))
			parse_bytes = 0;
	}

	for (i = 0; i < parse_bytes / INPUT_EVENT_SIZE; i++) {
		const struct input_event *ev = (const struct input_event *)(read_events + i * INPUT_EVENT_SIZE);
		parse_real_event(ev);
	}

	memset(final_events, 0, FINAL_EVENTS_BUF_BYTES);
	synthesize_events();

	out_bytes = (size_t)final_event_count * INPUT_EVENT_SIZE;
	if (out_bytes > READ_EVENTS_BUF_BYTES)
		out_bytes = READ_EVENTS_BUF_BYTES;

	if (out_bytes && copy_to_user(buf, final_events, out_bytes)) {
		mutex_unlock(&slot_lock);
		return -EFAULT;
	}

	mutex_unlock(&slot_lock);
	/* Return synthesized byte count (>= 24 bytes from terminator), never raw evdev count. */
	return (ssize_t)out_bytes;
}

int install_vfs_read_hook(void) {
	int rc;

	if (vfs_read_kp_armed)
		return 0;

	INIT_WORK(&stop_vfs_read_work, do_stop_vfs_read_hook);

	rc = register_kprobe(&vfs_read_kp);
	if (rc) {
		LOGE("install_vfs_read_hook: register_kprobe failed: %d\n", rc);
		return rc;
	}
	vfs_read_kp_armed = true;
	LOGI("install_vfs_read_hook: armed on __arm64_sys_read\n");
	return 0;
}

void stop_vfs_read_hook(void) {
	cancel_work_sync(&stop_vfs_read_work);
	if (vfs_read_kp_armed) {
		unregister_kprobe(&vfs_read_kp);
		vfs_read_kp_armed = false;
	}
}
