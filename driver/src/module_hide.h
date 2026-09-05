// SPDX-License-Identifier: GPL-2.0-only
#ifndef DRIVER_MODULE_HIDE_H
#define DRIVER_MODULE_HIDE_H

#if KCFG_HIDE_SELF_MODULE
int module_hide_arm(void);
void module_hide_disarm(void);
#else
static inline int module_hide_arm(void) { return 0; }
static inline void module_hide_disarm(void) { }
#endif

#endif /* DRIVER_MODULE_HIDE_H */
