#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
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
#include <sneks/thrd.h>
#include <sneks/process.h>
#include <sneks/console.h>
#include <sneks/systask.h>
#include <sneks/sys/sysmem-defs.h>
#include <sneks/sys/info-defs.h>
#include <sneks/sys/kmsg-defs.h>
#include <sneks/api/proc-defs.h>

#include "private.h"

static uintptr_t current_brk = 0, heap_bottom = 0;

const int __thrd_stksize_log2 = 16;
L4_ThreadId_t __uapi_tid = L4_anythread;
L4_KernelInterfacePage_t *__the_kip;

void __assert_failure(
	const char *cond, const char *file, unsigned int line, const char *fn)
{
	printf("!!! assert(%s) failed in `%s' [%s:%u]\n", cond, fn, file, line);
	abort();
}

int __thrd_new(L4_ThreadId_t *res) {
	return __proc_create_thread(__uapi_tid, &res->raw);
}

int __thrd_destroy(L4_ThreadId_t tid) {
	return __proc_remove_thread(__uapi_tid, tid.raw, L4_LocalIdOf(tid).raw);
}

noreturn void panic(const char *msg)
{
	printf("!!! PANIC: %s\n", msg);
	exit(666);
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


pid_t getpid(void) {
	return pidof_NP(L4_Myself());
}


int atexit(void (*fn)(void)) {
	/* TODO */
	return 0;
}


void exit(int status)
{
	int n = __proc_exit(__uapi_tid, status);
	printf("sys/crt:%s: Proc::exit failed, n=%d\n", __func__, n);
	for(;;) L4_Sleep(L4_Never);
}

long sysconf(int name)
{
	switch(name) {
		case _SC_PAGESIZE: return PAGE_SIZE;
		case _SC_NPROCESSORS_ONLN: return __the_kip->ProcessorInfo.X.processors + 1;
		case _SC_OPEN_MAX: return 0; /* systasks don't get fds */
		default: errno = EINVAL; return -1;
	}
}

L4_ThreadId_t __get_rootfs(void) {
	struct sneks_rootfs_info blk = { };
	int n = __info_rootfs_block(L4_Pager(), &blk);
	if(n != 0) { log_crit("can't get rootfs block: %s", stripcerr(n)); abort(); }
	return (L4_ThreadId_t){ .raw = blk.service };
}

int __crt1_entry(void)
{
	__the_kip = L4_GetKernelInterface();
	int *argc_p = (int *)((uintptr_t)__the_kip - PAGE_SIZE), argc = *argc_p;
	char *argbase = (char *)&argc_p[1], *argmem = argbase, *argv[argc + 1];
	for(int i = 0; i <= argc; i++) {
		argv[i] = argmem;
		argmem += strlen(argmem) + 1;
	}
	int arglen = argmem - argbase;
	char copy[arglen + 1];
	memcpy(copy, argbase, arglen + 1);
	for(int i = 0; i <= argc; i++) argv[i] += &copy[0] - argbase;
	/* throw away argmem, disregarding errors */
	for(uintptr_t addr = (uintptr_t)argc_p; addr <= (uintptr_t)argmem; addr += PAGE_SIZE) {
		__sysmem_send_virt(L4_Pager(), &(uint16_t){ 0 }, addr, L4_nilthread.raw, 0);
	}
	int n = sneks_setup_console_stdio();
	if(n < 0) return -n;
	struct sneks_uapi_info ui;
	if(__info_uapi_block(L4_Pager(), &ui) != 0) abort();
	__uapi_tid.raw = ui.service;
	assert(!L4_IsNilThread(__uapi_tid) && L4_IsGlobalId(__uapi_tid));
	__thrd_init();
	extern int main(int argc, char *const argv[], char *const envp[]);
	return main(argc, argv, NULL);
}
