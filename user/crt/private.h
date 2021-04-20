
/* bits and bobs of userspace runtime that aren't exported to the programs
 * it's linked to. initializers and so forth.
 */

#ifndef _SNEKS_USER_CRT_PRIVATE_H
#define _SNEKS_USER_CRT_PRIVATE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdnoreturn.h>
#include <setjmp.h>
#include <errno.h>
#include <ucontext.h>
#include <ccan/intmap/intmap.h>

#include <l4/types.h>
#include <l4/kip.h>

#include <sneks/sysinfo.h>


extern L4_KernelInterfacePage_t *__the_kip;
extern struct __sysinfo *__the_sysinfo;

extern L4_ThreadId_t __main_tid;

/* turns a muidl "positive for L4 ErrorCode values, negative for errno, zero
 * for success" style result into a written errno and a {0, -1} return value.
 * @n is stored in __l4_last_errorcode. the NTOERR() macro can be used to
 * avoid the double underscore, and to return a different positive value
 * instead of 0 (the second parameter).
 */
extern int __idl2errno(int n, ...);
#define NTOERR(...) __idl2errno(__VA_ARGS__, 0)

extern int __l4_last_errorcode;

struct fd_bits;
typedef SINTMAP(struct fd_bits *) fd_map_t;
extern fd_map_t fd_map;

extern int __cwd_fd;


struct sneks_fdlist;
extern void __file_init(struct sneks_fdlist *fdlist);


struct fd_bits
{
	L4_ThreadId_t server;
	intptr_t handle;
	uint8_t flags;
};


/* returns NULL when @fd isn't valid and sets errno to EBADF. */
extern struct fd_bits *__fdbits(int fd)
	__attribute__((pure));

/* creation. if @fd < 0, allocates the lowest available file descriptor.
 * otherwise if @fd is already valid, returns -EEXIST.
 */
extern int __create_fd(int fd, L4_ThreadId_t server, intptr_t handle, int flags);


/* from sigaction.c */
extern void __sig_bottom(void);
extern void __attribute__((regparm(3))) __sig_invoke(
	int sig, ucontext_t *uctx);
/* used from syscall wrappers that may wait on a receive phase for an extended
 * period and have an useful rollback path (waitid(2) under ~WNOHANG), or
 * require receive-phase signal interrupts as part of normal operation
 * (sigsuspend(2)). these assert against nesting and unpaired usage. by
 * default, aborts on the receive phase are forbidden.
 *
 * they're dressed up as functions to facilitate code reuse between this and a
 * future multithreaded runtime.
 */
extern void __permit_recv_interrupt(void);
extern void __forbid_recv_interrupt(void);


/* from threads.c (see comment in that file) */

/* returns muidl stub return. fills in *@tid_p. */
extern int __crt_thread_create(
	L4_ThreadId_t *tid_p, void (*fn)(void *), void *param_ptr);
extern void __crt_thread_join(L4_ThreadId_t tid);


/* from uid.c */
extern void __init_crt_cached_creds(void);


/* from setjmp-32.S */
extern noreturn void __longjmp_actual(jmp_buf, int);


/* from siginvoke-32.S */
extern void __attribute__((regparm(3))) __invoke_sig_sync(int);


#endif
