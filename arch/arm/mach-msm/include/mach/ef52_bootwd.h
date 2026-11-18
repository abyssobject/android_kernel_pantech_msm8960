/*
 * EF52 bring-up boot-hang watchdog (see arch/arm/mach-msm/ef52_bootwd.c)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 */
#ifndef __MACH_EF52_BOOTWD_H
#define __MACH_EF52_BOOTWD_H

#include <linux/init.h>

#ifdef CONFIG_EF52_BOOT_HANG_WD
struct task_struct;

extern initcall_t ef52_bootwd_cur_initcall;
extern unsigned long ef52_bootwd_cur_initcall_start;
extern struct task_struct *ef52_bootwd_cur_task;

/* machine init_early hook, after pantech_crash_log_early_init() */
void ef52_bootwd_early_init(void);
/* the regular MSM watchdog driver now owns (and pets) the hardware dog */
void ef52_bootwd_hwdog_handover(void);
void ef52_bootwd_check_restart(const char *cmd);
#else
static inline void ef52_bootwd_early_init(void) { }
static inline void ef52_bootwd_hwdog_handover(void) { }
static inline void ef52_bootwd_check_restart(const char *cmd) { }
#endif

#endif
