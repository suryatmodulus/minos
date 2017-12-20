/*
 * Created by Le Min 2017/12/12
 */

#include <asm/cpu.h>
#include <core/core.h>

extern int init_mem_block(void);
extern void init_pcpus(void);
extern void init_vms(void);
extern int log_buffer_init(void);
extern void sched_vcpu(void);

int boot_main(void)
{
	log_buffer_init();

	if (get_cpu_id() != 0)
		panic("cpu is not cpu0");

	init_mem_block();
	init_pcpus();
	init_vms();

	//wake_up_other_cpus();

	sched_vcpu();

	return 0;
}

int boot_secondary(void)
{
	sched_vcpu();
}