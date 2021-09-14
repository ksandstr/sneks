#ifdef __sneks__

#include <stdio.h>
#include <stdlib.h>
#include <ccan/minmax/minmax.h>
#include <ccan/array_size/array_size.h>
#include <sneks/crtprivate.h>

#define PRINT(x) printf(#x "=%#lx\n", (unsigned long)(x))

int main(int argc, char *argv[])
{
	char foo;
	extern char _start;
	PRINT(&foo);
	PRINT(&_start);
	size_t cands[] = {
		(size_t)__the_sysinfo, (size_t)__the_kip,
		(size_t)&foo, (size_t)&_start,
	};
	size_t botptr = cands[0];
	for(int i=1; i < ARRAY_SIZE(cands); i++) botptr = min(botptr, cands[i]);
	size_t botsize = botptr - 0x10000;
	PRINT(botsize);
	if(argc > 1) {
		char *endptr = NULL;
		uintptr_t address = strtoul(argv[1], &endptr, 0);
		if(endptr != argv[1]) *(volatile char *)address = 0x69;
	}
	return 0;
}

#else
int main(void) { return 0; }
#endif
