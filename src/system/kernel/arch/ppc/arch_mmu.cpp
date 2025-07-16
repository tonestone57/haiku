/*
** Copyright 2003, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
** Distributed under the terms of the MIT License.
*/


#include <KernelExport.h>
#include <arch_mmu.h>
#include <arch_cpu.h>
#include <stdio.h>


uint32
page_table_entry::PrimaryHash(uint32 virtualSegmentID, uint32 virtualAddress)
{
	dprintf("page_table_entry::PrimaryHash\n");
	return (virtualSegmentID & 0x7ffff) ^ ((virtualAddress >> 12) & 0xffff);
}


uint32
page_table_entry::SecondaryHash(uint32 virtualSegmentID, uint32 virtualAddress)
{
	dprintf("page_table_entry::SecondaryHash\n");
	return ~PrimaryHash(virtualSegmentID, virtualAddress);
}


uint32
page_table_entry::SecondaryHash(uint32 primaryHash)
{
	dprintf("page_table_entry::SecondaryHash\n");
	return ~primaryHash;
}


void
ppc_get_page_table(page_table_entry_group **_pageTable, size_t *_size)
{
	dprintf("ppc_get_page_table\n");
	uint32 sdr1 = get_sdr1();

	*_pageTable = (page_table_entry_group *)(sdr1 & 0xffff0000);
	*_size = ((sdr1 & 0x1ff) + 1) << 16;
}


void
ppc_set_page_table(page_table_entry_group *pageTable, size_t size)
{
	dprintf("ppc_set_page_table\n");
	set_sdr1(((uint32)pageTable & 0xffff0000) | (((size -1) >> 16) & 0x1ff));
}

