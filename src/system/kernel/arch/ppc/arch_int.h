#ifndef _SYSTEM_KERNEL_ARCH_PPC_ARCH_INT_H
#define _SYSTEM_KERNEL_ARCH_PPC_ARCH_INT_H

#include <arch_cpu.h>

#ifdef __cplusplus
extern "C" {
#endif

void debug_print_registers(struct iframe *iframe);

#ifdef __cplusplus
}
#endif

#endif /* _SYSTEM_KERNEL_ARCH_PPC_ARCH_INT_H */
