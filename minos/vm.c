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
#include <minos/vcpu.h>
#include <minos/virt.h>
#include <minos/vm.h>
#include <minos/vcpu.h>
#include <minos/vmm.h>
#include <minos/os.h>
#include <minos/sched.h>

extern void get_vcpu_affinity(int *aff, int nr);

static inline void vminfo_to_vmtag(struct vminfo *info, struct vmtag *tag)
{
	tag->vmid = VMID_INVALID;
	tag->name = (char *)info->name;
	tag->type = (char *)info->os_type;
	tag->nr_vcpu = info->nr_vcpus;
	tag->entry = info->entry;
	tag->setup_data = info->setup_data;
	tag->bit64 = info->bit64;

	/* for the dynamic need to get the affinity dynamicly */
	get_vcpu_affinity(tag->vcpu_affinity, tag->nr_vcpu);
}

int vm_power_up(int vmid)
{
	int i;
	struct vm *vm = get_vm_by_id(vmid);

	if (vm == NULL) {
		pr_error("vm-%d is not exist\n", vmid);
		return -ENOENT;
	}

	for (i = 0; i < vm->vcpu_nr; i++)
		vm->os->ops->vcpu_init(vm->vcpus[i]);

	return 0;
}

int create_new_vm(struct vminfo *info)
{
	int ret;
	struct vm *vm;
	size_t size;
	struct vmtag vme;
	struct vminfo *vminfo = (struct vminfo *)
			guest_va_to_pa((unsigned long)info, 0);

	if (!vminfo)
		return VMID_INVALID;

	/*
	 * first check whether there are enough
	 * memory for this vm
	 */
	size = vminfo->mem_end - vminfo->mem_start;
	if (size > vminfo->mem_size)
		size = vminfo->mem_size;

	if ((vminfo->mem_start + size) > GUSET_MEMORY_END)
		return -EINVAL;

	if (!has_enough_memory(size))
		return -ENOMEM;

	if (vminfo->nr_vcpus > NR_CPUS)
		return -EINVAL;

	memset(&vme, 0, sizeof(struct vmtag));
	vminfo_to_vmtag(vminfo, &vme);

	vm = create_dynamic_vm(&vme);
	if (!vm)
		return VMID_INVALID;

	/*
	 * allocate memory to this vm
	 */
	vm->bit64 = !!vminfo->bit64;
	vm_mm_struct_init(vm);

	ret = alloc_vm_memory(vm, vminfo->mem_start, size);
	if (ret)
		goto release_vm;

release_vm:
	destory_vm(vm);

	return ret;
}
