/*
 * EF52 (Pantech VEGA IRON, APQ8064) bring-up boot-hang watchdog.
 *
 * The device cannot be reset by key when the kernel hangs and has no
 * ram_console, so a boot that hangs before userspace is up leaves no trace:
 * the MSM watchdog driver only starts in a late initcall and the Pantech
 * bootloader only saves the kernel log (SKYLOG/LOGn/xxx.TXT on the "log"
 * partition) after an abnormal *reset*.
 *
 * Enabled only by the kernel command line:
 *
 *	ef52_kbootwd=<seconds>[,savelog][,hwdog]
 *
 * and disarmed by userspace with
 *
 *	echo 0 > /proc/ef52_kbootwd
 *
 * (writing N > 0 restarts the countdown with N seconds).  If it is not
 * disarmed in time it logs the initcall in progress, the interrupted
 * context, all CPU backtraces and the blocked tasks, then resets once:
 *
 * default	Clean restart into recovery, exactly what "reboot recovery"
 *		does on the 3.4 kernel (restart_reason 0x77665502, Pantech
 *		reason NORMAL, download cookie cleared).  Known to reach
 *		recovery; the bootloader does NOT save the log for a normal
 *		restart.
 *
 * savelog	panic(): the path that produced SKYLOG/.../ERRLINUX.TXT with
 *		the 3.4 kernel (Pantech reason LINUX, panic magic in the SMEM
 *		log header, download cookie set), with restart_reason set to
 *		"recovery" instead of the 0x77665501 a 3.4 panic writes.  The
 *		3.4 kernel never produced this combination, so whether the
 *		bootloader honours the recovery request after it saved the log
 *		is UNVERIFIED.  If it boots this kernel again, the Pantech
 *		reason word still names the crash (the 3.4 "silent boot" logic
 *		relies on that); in that case this boot falls back to the
 *		default mode and never arms the hardware watchdog, so there are
 *		at most two resets.
 *
 * hwdog	Additionally start the MSM hardware watchdog (11 s bark, 14 s
 *		bite, the values of the platform data) from the machine
 *		init_early hook and pet it from this driver's timer until the
 *		regular driver takes over or userspace disarms.  This covers
 *		hangs with interrupts off and hangs before the first initcall,
 *		but its reset cannot be steered into recovery: the device
 *		reboots normally.  It is skipped when the previous boot ended
 *		abnormally (see above) to avoid a reset loop.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 */

#define pr_fmt(fmt) "ef52_bootwd: " fmt

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/nmi.h>
#include <linux/reboot.h>
#include <linux/ptrace.h>
#include <linux/string.h>
#include <asm/irq_regs.h>
#include <mach/msm_iomap.h>
#include <mach/restart.h>
#include <mach/pantech_sys.h>
#include <mach/ef52_bootwd.h>
#include "msm_watchdog.h"

/* register layout and values as in msm_watchdog.c / the 8064 platform data */
#define EF52_WDT_BASE		(MSM_TMR0_BASE + WDT0_OFFSET)
#define EF52_WDT_RST		0x0
#define EF52_WDT_EN		0x8
#define EF52_WDT_BARK_TIME	0x14
#define EF52_WDT_BITE_TIME	0x24
#define EF52_WDT_HZ		32768
#define EF52_WDT_BARK_SECS	11
#define EF52_WDT_EXPIRED_ENABLE	(MSM_CLK_CTL_BASE + 0x3820)

enum { EF52_WD_RECOVERY, EF52_WD_SAVELOG };

initcall_t ef52_bootwd_cur_initcall;
unsigned long ef52_bootwd_cur_initcall_start;
struct task_struct *ef52_bootwd_cur_task;

static unsigned int wd_timeout;		/* seconds, 0 = feature off */
static int wd_mode = EF52_WD_RECOVERY;
static bool wd_hwdog_req;
static bool wd_prev_abnormal;
static bool wd_armed;
static unsigned long wd_deadline;
static struct timer_list wd_timer;

static bool hw_owned;			/* we started the hardware dog */
static bool hw_handed_over;		/* msm_watchdog.c pets it now */

extern void *restart_reason;

static char recovery_cmd[] = "recovery";

static int __init ef52_bootwd_setup(char *str)
{
	char *end;

	if (!str)
		return 0;
	wd_timeout = simple_strtoul(str, &end, 0);
	if (strstr(end, "savelog"))
		wd_mode = EF52_WD_SAVELOG;
	if (strstr(end, "hwdog"))
		wd_hwdog_req = true;
	return 0;
}
early_param("ef52_kbootwd", ef52_bootwd_setup);

static void hwdog_pet(void)
{
	__raw_writel(1, EF52_WDT_BASE + EF52_WDT_RST);
}

static void hwdog_stop(void)
{
	if (hw_owned && !hw_handed_over) {
		__raw_writel(0, EF52_WDT_BASE + EF52_WDT_EN);
		mb();
	}
	hw_owned = false;
}

void ef52_bootwd_hwdog_handover(void)
{
	hw_handed_over = true;
}

/* machine init_early: static mappings exist, nothing else does */
void __init ef52_bootwd_early_init(void)
{
	unsigned int r = prev_reset_reason;

	if (!wd_timeout)
		return;

	/* previous reset was not a clean restart: see the comment on top */
	if ((r & 0xFF000000) == SYS_RESET_REASON_MASK &&
	    (r & 0xF0) != (SYS_RESET_REASON_NORMAL_MASK & 0xF0)) {
		wd_prev_abnormal = true;
		wd_mode = EF52_WD_RECOVERY;
		pr_warn("previous reset was abnormal (0x%08X): recovery mode only, no hwdog\n",
			r);
	}

	if (wd_hwdog_req && !wd_prev_abnormal) {
		u32 bark = EF52_WDT_BARK_SECS * EF52_WDT_HZ;

		__raw_writel(0x1, EF52_WDT_EXPIRED_ENABLE);
		__raw_writel(bark, EF52_WDT_BASE + EF52_WDT_BARK_TIME);
		__raw_writel(bark + 3 * EF52_WDT_HZ,
			     EF52_WDT_BASE + EF52_WDT_BITE_TIME);
		__raw_writel(1, EF52_WDT_BASE + EF52_WDT_EN);
		__raw_writel(1, EF52_WDT_BASE + EF52_WDT_RST);
		mb();
		hw_owned = true;
		pr_warn("hardware watchdog started (%d s bark)\n",
			EF52_WDT_BARK_SECS);
		/*
		 * A watchdog bite cannot choose where the device goes, but
		 * the boot-mode word in IMEM is read by the bootloader after
		 * every warm reset.  Ask for recovery up front so that an
		 * early hang ends in TWRP if the bootloader honours the word
		 * after a watchdog reset (unverified).  A clean restart
		 * rewrites it in msm_restart().
		 */
		if (restart_reason) {
			__raw_writel(0x77665502, restart_reason);
			mb();
			pr_warn("boot-mode word preset to recovery\n");
		}
	}
}

static void ef52_bootwd_fire(void)
{
	struct pt_regs *regs = get_irq_regs();
	initcall_t fn = ef52_bootwd_cur_initcall;
	struct task_struct *t = ef52_bootwd_cur_task;

	wd_armed = false;
	console_verbose();

	pr_emerg("BOOT HANG: not disarmed within %u s (mode %s, system_state %d)\n",
		 wd_timeout, wd_mode == EF52_WD_SAVELOG ? "savelog" : "recovery",
		 system_state);
	if (fn)
		pr_emerg("initcall in progress: %pF, running for %u ms\n", fn,
			 jiffies_to_msecs(jiffies -
					  ef52_bootwd_cur_initcall_start));
	else
		pr_emerg("no synchronous initcall in progress\n");

	if (hw_owned && !hw_handed_over)
		hwdog_pet();

	if (regs) {
		pr_emerg("context interrupted on cpu%d:\n",
			 raw_smp_processor_id());
		show_regs(regs);
	}
	trigger_all_cpu_backtrace();
	if (t) {
		pr_emerg("task running the initcalls:\n");
		sched_show_task(t);
	}
	pr_emerg("blocked tasks:\n");
	show_state_filter(TASK_UNINTERRUPTIBLE);

	/* the dumps and the panic delay must not end in a watchdog bite */
	hwdog_stop();

	if (wd_mode == EF52_WD_SAVELOG) {
		msm_restart_crash_to_recovery = 1;
		if (!panic_timeout)
			panic_timeout = 1;
		panic("ef52 boot-hang watchdog: not disarmed within %u s",
		      wd_timeout);
	}

	pr_emerg("restarting into recovery\n");
	local_irq_disable();
	machine_restart(recovery_cmd);
}

static void ef52_bootwd_tick(unsigned long data)
{
	if (!wd_armed)
		return;

	if (hw_owned && !hw_handed_over)
		hwdog_pet();

	if (time_before(jiffies, wd_deadline)) {
		mod_timer(&wd_timer, jiffies + HZ);
		return;
	}

	ef52_bootwd_fire();
}

static int ef52_bootwd_show(struct seq_file *m, void *v)
{
	long left = 0;

	if (wd_armed)
		left = ((long)(wd_deadline - jiffies)) / HZ;
	seq_printf(m, "%s timeout=%u left=%ld mode=%s hwdog=%s prev_abnormal=%d\n",
		   wd_armed ? "armed" : "disarmed", wd_timeout, left,
		   wd_mode == EF52_WD_SAVELOG ? "savelog" : "recovery",
		   !hw_owned ? "off" : hw_handed_over ? "msm_watchdog" : "own",
		   wd_prev_abnormal);
	return 0;
}

static int ef52_bootwd_open(struct inode *inode, struct file *file)
{
	return single_open(file, ef52_bootwd_show, NULL);
}

static ssize_t ef52_bootwd_write(struct file *file, const char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	char buf[16];
	unsigned long secs;
	size_t n = min(count, sizeof(buf) - 1);

	if (copy_from_user(buf, ubuf, n))
		return -EFAULT;
	buf[n] = '\0';
	secs = simple_strtoul(buf, NULL, 0);

	if (!wd_armed)
		return count;

	if (secs) {
		wd_timeout = secs;
		wd_deadline = jiffies + secs * HZ;
		pr_info("countdown restarted: %lu s\n", secs);
		return count;
	}

	wd_armed = false;
	del_timer_sync(&wd_timer);
	hwdog_stop();
	pr_info("disarmed by %s/%d\n", current->comm, task_pid_nr(current));
	return count;
}

static const struct file_operations ef52_bootwd_fops = {
	.owner		= THIS_MODULE,
	.open		= ef52_bootwd_open,
	.read		= seq_read,
	.write		= ef52_bootwd_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init ef52_bootwd_start(void)
{
	if (!wd_timeout)
		return 0;

	if (!proc_create("ef52_kbootwd", S_IRUSR | S_IWUSR, NULL,
			 &ef52_bootwd_fops))
		pr_err("cannot create /proc/ef52_kbootwd\n");

	setup_timer(&wd_timer, ef52_bootwd_tick, 0);
	wd_deadline = jiffies + wd_timeout * HZ;
	wd_armed = true;
	mod_timer(&wd_timer, jiffies + HZ);

	pr_warn("armed: %u s, mode %s%s; disarm with 'echo 0 > /proc/ef52_kbootwd'\n",
		wd_timeout, wd_mode == EF52_WD_SAVELOG ? "savelog" : "recovery",
		hw_owned ? ", hwdog" : "");
	return 0;
}
early_initcall(ef52_bootwd_start);
