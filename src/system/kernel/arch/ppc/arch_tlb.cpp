/*
 * Copyright 2014, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2012, Alex Smith, alex@alex-smith.me.uk.
 * Copyright 2003-2011, Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2001, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */

#include <arch_mmu.h>
#include <arch_tlb.h>

#include <arch/int.h>
#include <boot/kernel_args.h>
#include <vm/vm.h>
#include <vm/VMAddressSpace.h>


#include <thread.h>

void
ppc_handle_tlb_miss(addr_t address, bool isWrite)
{
	dprintf("ppc_handle_tlb_miss\n");
	dprintf("TLB miss: address %p, %s, thread %" B_PRId32 "\n", (void*)address,
		isWrite ? "write" : "read", thread_get_current_thread_id());

	uint32 flags;
	phys_addr_t physicalAddress;

	dprintf("TLB miss: arch_mmu_query\n");
	status_t error = arch_mmu_query(address, &physicalAddress, &flags);
	if (error != B_OK) {
		dprintf("TLB miss: arch_mmu_query failed\n");
		return;
	}

	if ((flags & PAGE_PRESENT) == 0) {
		dprintf("TLB miss: page not present\n");
		return;
	}

	// TODO: Write protect pages.
	dprintf("TLB miss: arch_mmu_map_page\n");
	arch_mmu_map_page(address, physicalAddress, flags, NULL);
	dprintf("ppc_handle_tlb_miss done\n");
}
