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
#include <sys/stat.h>
#include <ccan/intmap/intmap.h>
#include <l4/types.h>
#include <l4/kip.h>

#define MAX_FD ((1 << 15) - 1)	/* TODO: get from sysconf(), stash somewhere */

struct fd_bits {
	L4_ThreadId_t server;
	int handle, flags;
};

struct fd_ext {
	struct stat st;
};

struct resolve_out {
	L4_ThreadId_t server;
	unsigned object;
	L4_Word_t cookie;
	int ifmt;
};

typedef struct {
	int a_type;
	union {
		size_t a_val;
		void *a_ptr;
		void (*a_fnc)();
	};
} auxv_t;

struct __sysinfo;
struct sneks_path_statbuf;
typedef SINTMAP(struct fd_bits *) fd_map_t;
typedef SINTMAP(struct fd_ext *) fdext_map_t;

extern L4_KernelInterfacePage_t *__the_kip;
extern struct __sysinfo *__the_sysinfo;
extern L4_ThreadId_t __main_tid;
extern int __cwd_fd;
extern fd_map_t fd_map;
extern fdext_map_t __fdext_map;

/* turns a muidl "positive for L4 ErrorCode values, negative for errno, zero
 * for success" style result into a written errno and a {0, -1} return value.
 * the NTOERR() comfort macro allows return of a different positive value.
 */
extern int __idl2errno(int n, ...);
#define NTOERR(...) __idl2errno(__VA_ARGS__, 0)

extern void __file_init(const size_t *pp);

/* NULL when @fd isn't valid. */
static inline struct fd_bits *__fdbits(int fd) {
	return fd < 0 || fd > MAX_FD ? NULL : sintmap_get(&fd_map, fd);
}

static inline struct fd_ext *__fdext(int fd) { return sintmap_get(&__fdext_map, fd); }

/* creation. if @fd < 0, allocates the lowest available file descriptor.
 * otherwise if @fd is already valid, returns -EEXIST. @flags may contain
 * FD_CLOEXEC, returns -EINVAL for invalid flags set. _ext variant adds
 * __fdext() values as well iff @fs != nil.
 */
extern int __create_fd(int fd, L4_ThreadId_t server, int handle, int flags);
extern int __create_fd_ext(int fd, L4_ThreadId_t server, intptr_t handle, int flags, const struct stat *st);

/* from path.c */
extern int __resolve(struct resolve_out *result, int dirfd, const char *pathname, int flags);

/* from stat.c */
extern void __convert_statbuf(struct stat *dst, const struct sneks_path_statbuf *src);

/* from sigaction.c */
extern void __sig_bottom(void);
extern void __attribute__((regparm(3))) __sig_invoke(int sig, ucontext_t *uctx);
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
extern int __crt_thread_create(L4_ThreadId_t *tid_p, void (*fn)(void *), void *param_ptr);
extern void __crt_thread_join(L4_ThreadId_t tid);

/* from uid.c */
extern void __init_crt_cached_creds(const size_t *flat_auxv);

/* from setjmp-32.S */
extern noreturn void __longjmp_actual(jmp_buf, int);

/* from siginvoke-32.S */
extern void __attribute__((regparm(3))) __invoke_sig_sync(int);

#endif
