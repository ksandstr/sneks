
/* bits and bobs of userspace runtime that aren't exported to the programs
 * it's linked to. initializers and so forth.
 */

#ifndef _SNEKS_USER_CRT_PRIVATE_H
#define _SNEKS_USER_CRT_PRIVATE_H

#include <stdbool.h>
#include <stdnoreturn.h>
#include <setjmp.h>
#include <errno.h>
#include <ucontext.h>
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


struct sneks_fdlist;
extern void __file_init(struct sneks_fdlist *fdlist);


struct fdchunk;
struct fd_iter {
	struct fdchunk *chunk;
};


/* fd-to-component access w/ group caching via @ctx (initialize to NULL,
 * invalidated by next call to close()).
 */
extern L4_ThreadId_t __server(void **ctx, int fd);
extern L4_Word_t __handle(void **ctx, int fd);
extern int __fflags(void **ctx, int fd); /* file descriptor (FD_*) flags */
extern bool __fd_valid(void **ctx, int fd);

/* creation. if @fd < 0, allocates a different file descriptor. otherwise if
 * @fd is already valid, returns -EEXIST.
 */
extern int __alloc_fd(
	void **ctx, int fd,
	L4_ThreadId_t server, L4_Word_t handle, int flags);

/* these return -1 when there were no file descriptors, or when @prev was the
 * last one, respectively. __fd_iter_ctx() returns an useful value for @ctx in
 * __server etc.
 */
extern int __fd_first(struct fd_iter *it);
extern int __fd_next(struct fd_iter *it, int prev);
static inline void *__fd_iter_ctx(struct fd_iter *it) {
	return it->chunk;
}


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
