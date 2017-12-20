/*
 * created by Le MIn 2017/12/09
 */

#include <core/vm.h>
#include <asm/cpu.h>
#include <asm/v8_system.h>

static int os1_boot_vm(struct vmm_vcpu_context *c)
{
	return 0;
}

struct vmm_vm_entry vm_os1 = {
	.name 		= "os1",
	.ram_base 	= 0x9000000,
	.ram_size 	= (256 * 1024 *1024),
	.nr_vcpu 	= 4,
	.entry_point 	= 0x90000000,
	.vcpu_affinity 	= {0},
	.boot_vm 	= os1_boot_vm,
};

register_vm_entry(vm_os1);