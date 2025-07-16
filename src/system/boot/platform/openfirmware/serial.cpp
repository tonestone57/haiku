#include "serial.h"
#include "openfirmware.h"

static int sScreen;

void
serial_init(void)
{
	if ((sScreen = of_finddevice("screen")) == OF_FAILED)
		sScreen = -1;
}

void
serial_puts(const char* string)
{
	if (sScreen == -1)
		return;

	while (*string != '\0') {
		if (*string == '\n')
			of_write(sScreen, "\r\n", 2);
		else
			of_write(sScreen, string, 1);
		string++;
	}
}
