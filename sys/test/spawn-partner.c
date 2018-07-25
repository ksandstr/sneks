
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>
#include <ccan/array_size/array_size.h>

#include <l4/types.h>
#include <l4/ipc.h>
#include <l4/kdebug.h>

#include <sneks/mm.h>
#include <sneks/sysinfo.h>

#include "proc-defs.h"


L4_KernelInterfacePage_t *__the_kip = NULL;
struct __sysinfo *__the_sysinfo = NULL;


void __return_from_main(void)
{
	int n = __proc_exit(__the_sysinfo->api.proc, 0);
	printf("__proc_exit() returned n=%d\n", n);
	for(;;) {
		/* desperate times. */
		asm volatile ("int $69");
		L4_Sleep(L4_Never);
	}
}


void *sbrk(intptr_t delta) {
	return NULL;
}


void abort(void)
{
	L4_KDB_PrintString("aborted!");
	for(;;) {
		asm volatile ("int $70");
		L4_Sleep(L4_TimePeriod(10000));
	}
}


void __assert_failure(
	const char *cond,
	const char *file, unsigned int line, const char *fn)
{
	printf("!!! assert(%s) failed in `%s' [%s:%u]\n", cond, fn, file, line);
	abort();
}


void malloc_panic(void) {
	L4_KDB_PrintString("malloc_panic() called!");
	abort();
}


void con_putstr(const char *str) {
	L4_KDB_PrintString((char *)str);
}


void __crt1_entry(void)
{
	extern char _end;
#if 0
	extern char _start;
	int acc = 0;
	for(L4_Word_t a = (L4_Word_t)&_start & ~PAGE_MASK;
		a < (L4_Word_t)&_end;
		a += PAGE_SIZE)
	{
		volatile char *z = (void *)a;
		acc += *z;
	}
#endif

	static char dummeh[30];
	strscpy(dummeh, "hi im a dumy!!", sizeof dummeh);
	printf("user program making noise!\n");

	__the_kip = L4_GetKernelInterface();
	__the_sysinfo = __get_sysinfo(__the_kip);

	L4_Word_t argpos = ((L4_Word_t)&_end + PAGE_SIZE - 1) & ~PAGE_MASK;
	char *argv[12], *arg = (char *)argpos;
	for(int i=0; i < ARRAY_SIZE(argv); i++) {
		if(*arg != '\0') {
			printf("argv[%d]=`%s'\n", i, arg);
			argv[i] = arg;
			arg += strlen(arg) + 1;
		} else {
			argv[i] = NULL;
			break;
		}
	}
	/* we don't care about envbuf for now, as there are no tests for that
	 * anyway.
	 */
	L4_ThreadId_t oth = { .raw = strtoul(argv[1], NULL, 0) };
	if(L4_IsNilThread(oth)) {
		printf("oth parsed to nil?\n");
		return;
	}

	L4_LoadMR(0, 0);
	L4_MsgTag_t tag = L4_Call_Timeouts(oth, L4_TimePeriod(3000), L4_Never);
	if(L4_IpcFailed(tag)) {
		printf("call failed, ec=%lu\n", L4_ErrorCode());
	}
}
