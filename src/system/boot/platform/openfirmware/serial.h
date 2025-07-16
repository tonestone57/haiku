#ifndef _SYSTEM_BOOT_PLATFORM_OPENFIRMWARE_SERIAL_H
#define _SYSTEM_BOOT_PLATFORM_OPENFIRMWARE_SERIAL_H

#include <SupportDefs.h>

#ifdef __cplusplus
extern "C" {
#endif

void serial_init(void);
void serial_puts(const char* string);

#ifdef __cplusplus
}
#endif

#endif /* _SYSTEM_BOOT_PLATFORM_OPENFIRMWARE_SERIAL_H */
