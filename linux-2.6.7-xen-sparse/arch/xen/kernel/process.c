
#include <stdarg.h>

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/platform.h>
#include <linux/pm.h>


void xen_cpu_idle (void)
{
	local_irq_disable();
	if (need_resched()) {
		local_irq_enable();
		return;
	}
	if (0 && set_timeout_timer() == 0) {
		/* NB. Blocking reenable events in a race-free manner. */
		HYPERVISOR_block();
		return;
	}
	local_irq_enable();
	HYPERVISOR_yield();
}
