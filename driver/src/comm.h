// SPDX-License-Identifier: GPL-2.0
// userspace communication bootstrap: reboot () magic-handshake -> anon-inode fd.
#ifndef DRIVER_COMM_H
#define DRIVER_COMM_H

#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kprobes.h>
#include <linux/task_work.h>
#include <linux/types.h>

#include <driver/uapi.h>

#define COMM_REBOOT_MAGIC1 DRIVER_REBOOT_MAGIC1
#define COMM_REBOOT_MAGIC2 DRIVER_REBOOT_MAGIC2

struct driver_install_work {
	struct callback_head head;
	void __user *reply; /* +0x10: user pointer where fd is written */
};

int reboot_handler_pre(struct kprobe *p, struct pt_regs *regs);

/* Call from init_driver() before arming the reboot kprobe. */
int comm_warm_symbols(void);

void driver_install_fd_tw_func(struct callback_head *twork);

long dispatch_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

extern const struct file_operations inofile_fops;

extern struct kprobe reboot_kp;

#endif /* DRIVER_COMM_H */
