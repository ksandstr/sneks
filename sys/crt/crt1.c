
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <assert.h>
#include <threads.h>
#include <errno.h>
#include <unistd.h>

#include <ccan/likely/likely.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>
#include <l4/space.h>
#include <l4/kip.h>
#include <l4/kdebug.h>

#include <sneks/mm.h>
#include <sneks/process.h>
#include <sneks/console.h>
#include <sneks/sys/sysmem-defs.h>
#include <sneks/sys/info-defs.h>
#include <sneks/sys/kmsg-defs.h>

#include "private.h"


L4_ThreadId_t __rootfs_tid = { .raw = 0 };

static tss_t errno_key;
static uintptr_t current_brk = 0, heap_bottom = 0;


void __return_from_main(int main_rc)
{
	int n = __sysmem_rm_task(L4_Pager(), getpid());
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


noreturn void panic(const char *msg)
{
	printf("!!! PANIC: %s\n", msg);
	for(;;) { L4_Sleep(L4_Never); }
}


void malloc_panic(void) {
	panic("sys/crt: malloc_panic() called!");
}


void abort(void) {
	panic("aborted");
}


/* for con_stdio.c */
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

	char tmp[SNEKS_KMSG_MAX_LINE];
	while(*string != '\0') {
		const char *end = strchr(string, '\n');
		int n;
		if(end == NULL) n = __kmsg_putstr(kmsg_tid, string);
		else {
			end++;	/* include the line feed */
			int len = min_t(int, sizeof tmp - 1, end - string);
			memcpy(tmp, string, len);
			tmp[len] = '\0';
			n = __kmsg_putstr(kmsg_tid, tmp);
			string += len;
		}

		if(n != 0) {
			snprintf(tmp, sizeof tmp,
				"con_putstr: Kmsg::putstr failed, n=%d", n);
			L4_KDB_PrintString(tmp);
			break;
		}
		if(end == NULL) break;
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


int atexit(void (*fn)(void)) {
	/* does nothing since systasks don't exit in a conventional sense. */
	return 0;
}


int getpid(void) {
	return pidof_NP(L4_Myself());
}


void exit(int status)
{
	int n = __sysmem_rm_task(L4_Pager(), getpid());
	printf("%s: Sysmem::rm_task() returned n=%d\n", __func__, n);
	for(;;) abort();
}


/* for dlmalloc (maybe?) */
long sysconf(int name)
{
	switch(name) {
		case _SC_PAGESIZE: return PAGE_SIZE;
		case _SC_NPROCESSORS_ONLN: return 1;	/* FIXME: get from KIP! */
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

	/* throw away argmem */
	for(uintptr_t addr = (uintptr_t)argc_p;
		addr <= (uintptr_t)argmem;
		addr += PAGE_SIZE)
	{
		uint16_t rv;
		__sysmem_send_virt(L4_Pager(), &rv, addr, L4_nilthread.raw, 0);
		/* disregard errors. */
	}

	int n = sneks_setup_console_stdio();
	if(unlikely(n < 0)) {
		/* blind dumb dead! */
		return -n;
	}

	__thrd_init();

	struct sneks_rootfs_info blk;
	n = __info_rootfs_block(L4_Pager(), &blk);
	if(unlikely(n != 0)) {
		printf("crt1: can't get rootfs block, n=%d!\n", n);
		return 1;
	}
	__rootfs_tid.raw = blk.service;

	extern int main(int argc, char *const argv[], char *const envp[]);
	return main(argc, argv, NULL);
}
