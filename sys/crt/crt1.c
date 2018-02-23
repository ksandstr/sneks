
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <threads.h>
#include <errno.h>
#include <unistd.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>
#include <l4/space.h>
#include <l4/kip.h>
#include <l4/kdebug.h>

#include <sneks/mm.h>

#include "sysmem-defs.h"
#include "info-defs.h"
#include "kmsg-defs.h"


static tss_t errno_key;
static uintptr_t current_brk = 0, heap_bottom = 0;


void __return_from_main(int main_rc)
{
	int n = __sysmem_rm_thread(L4_Pager(), L4_Myself().raw, L4_Myself().raw);
	assert(n != 0);
	L4_KDB_PrintString("systask __return_from_main failed!");
	for(;;) { L4_Sleep(L4_Never); }
}


void __assert_failure(
	const char *cond, const char *file, unsigned int line, const char *fn)
{
	printf("!!! assert(%s) failed in `%s' [%s:%u]\n", cond, fn, file, line);
	abort();
}


static void init_errno_tss(void)
{
	int n = tss_create(&errno_key, &free);
	if(n != thrd_success) {
		printf("!!! %s can't initialize\n", __func__);
		abort();
	}
}


int *__errno_location(void)
{
	/* FIXME: slow and shitty. replace with _Thread int errno because the
	 * systask runtime is always multithreaded.
	 */
	static once_flag errno_init_flag = ONCE_FLAG_INIT;
	call_once(&errno_init_flag, &init_errno_tss);
	int *val = tss_get(errno_key);
	if(val == NULL) {
		val = malloc(sizeof *val);
		if(val == NULL) {
			printf("!!! %s can't initialize\n", __func__);
			abort();
		}
		*val = 0;
		tss_set(errno_key, val);
	}
	return val;
}


void NORETURN panic(const char *msg)
{
	printf("!!! PANIC: %s\n", msg);
	for(;;) { L4_Sleep(L4_Never); }
}


void malloc_panic(void) {
	panic("malloc_panic() called!");
}


void abort(void) {
	panic("aborted");
}


/* for fake_stdio.c */
static L4_ThreadId_t kmsg_tid = L4_nilthread;

static void init_kmsg_tid(void)
{
	L4_ThreadId_t sysinfo_tid;
	int n = __info_lookup(L4_Pager(), &sysinfo_tid.raw);
	if(n != 0) {
		L4_KDB_PrintString("crt1: __info_lookup() failed");
		return;		/* FIXME: ugly. */
	}

	struct sneks_kmsg_info info;
	n = __info_kmsg_block(sysinfo_tid, &info);
	if(n != 0) {
		L4_KDB_PrintString("crt1: __info_kmsg_block() failed");
		return;		/* FIXME: ugly. */
	}

	kmsg_tid.raw = info.service;
}


void con_putstr(const char *string)
{
	if(L4_IsNilThread(kmsg_tid)) {
		static once_flag kmsg_once = ONCE_FLAG_INIT;
		call_once(&kmsg_once, &init_kmsg_tid);
	}

	int n = __kmsg_putstr(kmsg_tid, string);
	if(n != 0) {
		char buf[100];
		snprintf(buf, sizeof buf, "con_putstr: Kmsg::putstr failed, n=%d", n);
		L4_KDB_PrintString(buf);
	}
}


/* mainly for runtime initialization in `vm'. */
int brk(void *ptr)
{
	uintptr_t addr = ((uintptr_t)ptr + PAGE_MASK) & ~PAGE_MASK;
	if(heap_bottom == 0) heap_bottom = addr;
	current_brk = addr;
	int n = __sysmem_brk(L4_Pager(), current_brk);
	if(n != 0) {
		errno = n < 0 ? -n : ENOSYS;	/* NOTE: questionable. */
		n = -1;
	}
	return n;
}


void *sbrk(intptr_t increment)
{
	if(current_brk == 0) {
		extern char _end;
		current_brk = ((L4_Word_t)&_end + PAGE_MASK) & ~PAGE_MASK;
		heap_bottom = current_brk;
		assert(current_brk != 0);
	}

	void *ret = (void *)current_brk;
	if(increment > 0) {
		current_brk = (current_brk + increment + PAGE_MASK) & ~PAGE_MASK;
	} else if(increment < 0) {
		increment = (-increment + PAGE_MASK) & ~PAGE_MASK;
		current_brk -= increment;
	}
	if(increment != 0) {
		if(current_brk < heap_bottom) current_brk = heap_bottom;
		int n = __sysmem_brk(L4_Pager(), current_brk);
		if(n != 0) {
			printf("sbrk failed, n=%d\n", n);
			abort();
		}
	}

	return ret;
}


/* ultra stubbery. getpid() is only used by dlmalloc, and even in there for no
 * good reason. but the stub must remain.
 */
int getpid(void) {
	return 666;
}


int atexit(void (*fn)(void)) {
	/* does nothing since systasks don't exit in a conventional sense. */
	return 0;
}


void exit(int status)
{
	/* TODO: do something */
	printf("%s: called! aborting.\n", __func__);
	for(;;) abort();
}


char *getenv(const char *name)
{
	/* simplest conforming implementation there is. */
	return NULL;
}


/* for dlmalloc (maybe?) */
long sysconf(int name)
{
	switch(name) {
		case _SC_PAGESIZE: return PAGE_SIZE;
		default: errno = EINVAL; return -1;
	}
}


int __crt1_entry(void)
{
	void *kip = L4_GetKernelInterface();
	int32_t *argc_p = (int32_t *)(kip - PAGE_SIZE), argc = *argc_p;
	char *argbase = (char *)&argc_p[1], *argmem = argbase;
	char *argv[argc + 1];
	for(int i=0; i <= argc; i++) {
		argv[i] = argmem;
		argmem += strlen(argmem) + 1;
	}
	int arglen = argmem - argbase;
	char copy[arglen + 1];
	memcpy(copy, argbase, arglen + 1);
	for(int i=0; i <= argc; i++) argv[i] += &copy[0] - argbase;

	/* call sysmem to throw away argmem */
	for(uintptr_t addr = (uintptr_t)argc_p;
		addr <= (uintptr_t)argmem;
		addr += PAGE_SIZE)
	{
		uint16_t rv;
		__sysmem_send_virt(L4_Pager(), &rv, addr, L4_nilthread.raw, 0);
		/* disregard errors due to no possible way to handle. it's an
		 * optimization anyway.
		 */
	}

	extern int main(int argc, char *const argv[], char *const envp[]);
	return main(argc, argv, NULL);
}
