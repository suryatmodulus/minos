/*
 * Copyright (C) 2018 Min Le (lemin9538@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <minos/minos.h>
#include <asm/arch.h>
#include <minos/sched.h>
#include <minos/platform.h>
#include <minos/irq.h>
#include <minos/mm.h>

extern void apps_cpu0_init(void);
extern void apps_cpu1_init(void);
extern void apps_cpu2_init(void);
extern void apps_cpu3_init(void);
extern void apps_cpu4_init(void);
extern void apps_cpu5_init(void);
extern void apps_cpu6_init(void);
extern void apps_cpu7_init(void);

static void create_static_tasks(void)
{
	int ret = 0, cpu;
	struct task_desc *tdesc;
	extern unsigned char __task_desc_start;
	extern unsigned char __task_desc_end;

	section_for_each_item(__task_desc_start, __task_desc_end, tdesc) {
		if (tdesc->aff == PCPU_AFF_PERCPU) {
			for_each_online_cpu(cpu) {
				ret = create_task(tdesc->name, tdesc->func,
						tdesc->arg, OS_PRIO_PCPU,
						cpu, tdesc->stk_size,
						tdesc->flags);
				if(ret) {
					pr_err("create [%s] fail on cpu-%d\n",
							tdesc->name, cpu);
				}
			}
		} else {
			ret = create_task(tdesc->name, tdesc->func,
					tdesc->arg, tdesc->prio,
					tdesc->aff, tdesc->stk_size,
					tdesc->flags);
			if (ret) {
				pr_err("create [%s] fail on cpu-%d@%d\n",
					tdesc->name, tdesc->aff, tdesc->prio);
			}

		}
	}
}

void system_reboot(void)
{
	if (platform->system_reboot)
		platform->system_reboot(0, NULL);

	panic("can not reboot system now\n");
}

void system_shutdown(void)
{
	if (platform->system_shutdown)
		platform->system_shutdown();

	panic("cant not shutdown system now\n");
}

int system_suspend(void)
{
	if (platform->system_suspend)
		platform->system_suspend();

	wfi();

	return 0;
}

int pcpu_can_idle(struct pcpu *pcpu)
{
	if (!sched_can_idle(pcpu))
		return 0;

	if (need_resched())
		return 0;

	return 1;
}

static void os_clean(void)
{
	/* recall the memory for init function and data */
	extern unsigned char __init_start;
	extern unsigned char __init_end;
	unsigned long size;

	size = (unsigned long)&__init_end -
		(unsigned long)&__init_start;
	pr_info("release unused memory [0x%x 0x%x]\n",
			(unsigned long)&__init_start, size);

	add_slab_mem((unsigned long)&__init_start, size);
}

void cpu_idle(void)
{
	struct pcpu *pcpu = get_cpu_var(pcpu);

	switch (pcpu->pcpu_id) {
	case 0:
		apps_cpu0_init();
		create_static_tasks();
		os_clean();
		break;
	case 1:
		apps_cpu1_init();
		break;
	case 2:
		apps_cpu2_init();
		break;
	case 3:
		apps_cpu3_init();
		break;
	case 4:
		apps_cpu4_init();
		break;
	case 5:
		apps_cpu5_init();
		break;
	case 6:
		apps_cpu6_init();
		break;
	case 7:
		apps_cpu7_init();
		break;
	default:
		pr_warn("cpu specfical init function not defined\n");
		break;
	}

	while (1) {
		sched();

		/*
		 * need to check whether the pcpu can go to idle
		 * state to avoid the interrupt happend before wfi
		 */
		local_irq_disable();
		if (pcpu_can_idle(pcpu)) {
			pcpu->state = PCPU_STATE_IDLE;
			local_irq_enable();
			wfi();
			dsb();
			pcpu->state = PCPU_STATE_RUNNING;
		} else
			local_irq_enable();
	}
}
