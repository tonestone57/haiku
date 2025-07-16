/*
 * Copyright 2014, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_ARCH_PPC_ARCH_TLB_H
#define _KERNEL_ARCH_PPC_ARCH_TLB_H

#include <SupportDefs.h>

void ppc_handle_tlb_miss(addr_t address, bool isWrite);

#endif /* _KERNEL_ARCH_PPC_ARCH_TLB_H */
