// SPDX-License-Identifier: GPL-2.0
// process memory read/write primitives (linear-map + vmap variants).
#ifndef DRIVER_MEMORY_H
#define DRIVER_MEMORY_H

#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/uaccess.h>

/* Walks @mm->pgd with folded-level pgtable helpers; honours PUD/PMD/PTE leaves. */
/* Merges the granule's PAGE_SIZE offset into the PA. Caller holds mmap_read_lock(mm). */
int vaddr_to_phys(struct mm_struct *mm, u64 va, u64 *out_phys);

/* Linear-map (phys_to_virt) variants; clean+invalidate dcache around the access via FlushDCache.dcache_line_size. */
int read_process_memory_linear(struct mm_struct *target_mm, u64 target_va, void *local_kbuf, size_t len);
int write_process_memory_linear(struct mm_struct *target_mm, u64 target_va, const void *local_kbuf, size_t len);

/* vmap variants: required for pages whose linear-map alias is RO or not-present (some CMA / ION regions). */
int read_process_memory_vmap(struct mm_struct *target_mm, u64 target_va, void *local_kbuf, size_t len);
int write_process_memory_vmap(struct mm_struct *target_mm, u64 target_va, const void *local_kbuf, size_t len);

int kernel_rw(u64 kva, void *buf, size_t len, int do_write);

int multi_read_process_memory(struct mm_struct *target_mm, void __user *descs, unsigned int count);

/* Resolve aarch64_insn_patch_text_nosync + get_cmdline via kallsyms after kallsym_init(). */
/* Missing symbols non-fatal: text write uses legacy fallback; package lookup returns -EOPNOTSUPP. */
int memory_init(void);

/* Patch kernel text via aarch64_insn_patch_text_nosync (FIX_TEXT_POKE0 fixmap). */
/* Legacy PGD/AP[2]/DBM fallback exists only for PAGE_SHIFT=12, 3/4 levels, no PTE_CONT block. */
/* Not stop_machine'd — caller ensures target VA is quiescent. */
u64 write_ro_memory(u64 dst_kva, const void *src, u64 len);

u64 process_get_module_base(struct task_struct *task, const char *module_name);

/* DRV_CMD_READ_VMA_COOKIE(0x11): matches anon_vma_name(vma) against @needle, returns vma->vm_start. */
u64 process_read_vma_cookie(struct task_struct *task, const char *needle);

/* Reads task->thread.uw.tp_value (saved TPIDR_EL0 for non-current tasks). */
u64 process_get_tls(struct task_struct *task);

/* Read target APGA keys; output is a best-effort snapshot. */
int process_get_apga(struct task_struct *task, u64 *lo, u64 *hi);

/* Searches all threads. Caller must put_task_struct() on non-NULL return. */
struct task_struct *process_find_task_by_comm(const char *comm);

/* Exact argv[0] lookup; returns smallest TGID in the caller's PID namespace. */
/* get_cmdline() may sleep, so tasks are snapshot under RCU and scanned outside it. */
int process_find_pid_by_package(const char *package, pid_t *out_pid);

int process_maps_get_a(struct task_struct *task, void __user *u_buf, size_t cap);

#endif /* DRIVER_MEMORY_H */
