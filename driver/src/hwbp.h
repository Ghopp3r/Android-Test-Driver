// SPDX-License-Identifier: GPL-2.0-only
#ifndef DRIVER_HWBP_H
#define DRIVER_HWBP_H

#include <linux/types.h>

struct file;

int hwbp_init(void);
long do_hwbp_cmd(unsigned int cmd, void __user *arg, struct file *owner);
long do_hwbp_ext_cmd(unsigned int cmd, void __user *arg);
void hwbp_clear_by_file(struct file *f);

#endif /* DRIVER_HWBP_H */
