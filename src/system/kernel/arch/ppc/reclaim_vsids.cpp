/*
 * Copyright 2014, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */

#include <arch_mmu.h>

#include <vm/vm.h>
#include <vm/VMAddressSpace.h>

extern "C" void reclaim_vsids();

void
reclaim_vsids()
{
	// This is a placeholder implementation. A real implementation would
	// iterate through all address spaces and reclaim VSIDs from those that
	// have been destroyed.
	panic("reclaim_vsids(): not implemented");
}
