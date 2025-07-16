#include <stdio.h>

extern "C" void arch_exceptions_debug_print(const char* message)
{
	dprintf(message);
}
