
#ifndef _SYS_IO_PRIVATE_H
#define _SYS_IO_PRIVATE_H

#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <ccan/darray/darray.h>

#include <sneks/io.h>


/* from io.c */

#define IOF_T(file) ((iof_t *)(file)->impl)
#define IO_FILE(impl) iof2f((impl))

#define CF_WRITE_BLOCKED 1
#define CF_NOTIFY 2

/* <struct fd>.flags:
 *   - 31..28 are EPOLL{EXCLUSIVE,WAKEUP,ONESHOT,ET};
 *   - 13 is EPOLLRDHUP
 *   - 4..0 are EPOLL{HUP,ERR,OUT,PRI,IN}
 *
 * this leaves:
 */
#define IOD_SHADOW (1 << 26)	/* shadow descriptor created at fork */
#define IOD_TRANSFER (1 << 27)	/* associated with one <struct fd_transfer> */
#define IOD_EPOLL_MASK (EPOLLEXCLUSIVE | EPOLLWAKEUP | EPOLLONESHOT | EPOLLET \
	| EPOLLRDHUP | EPOLLHUP | EPOLLERR | EPOLLOUT | EPOLLPRI | EPOLLIN)
#define IOD_PRIVATE_MASK (IOD_SHADOW | IOD_TRANSFER | IOD_EPOLL_MASK)


struct rangealloc;
struct fd;

struct io_file {
	darray(struct fd *) handles;
	int flags;
	char impl[] __attribute__((aligned(16)));
};


struct client
{
	unsigned short pid;
	darray(struct fd *) handles;
	union {
		L4_ThreadId_t blocker;		/* when ~flags & CF_NOTIFY */
		L4_ThreadId_t notify_tid;	/* when  flags & CF_NOTIFY */
	};
	int flags;	/* CF_* */

	/* descriptor translation table. fork(2) copies descriptors but they
	 * retain their values in the resulting child, so when the child goes to
	 * reference an IO::handle it'll either look like it belongs to the parent
	 * process or like it's been removed. to fix this, we allocate a
	 * translation table and access it in get_fd() when the ra_id2ptr() owner
	 * check fails.
	 */
	int fd_table_len;
	int fd_table[];
};


struct fd {
	struct io_file *file;
	struct client *owner;
	int flags;	/* IOD_*, EPOLL* */
	unsigned short file_ix, client_ix;
};


extern pid_t my_pid;
extern struct rangealloc *fd_ra;


static inline struct io_file *iof2f(iof_t *iof) {
	return (void *)iof - offsetof(struct io_file, impl);
}

extern int _nopoll_add_blocker(struct fd *f, L4_ThreadId_t tid, bool writing);
extern struct fd *get_fd(pid_t pid, int fdno);


/* from pollimpl.c */

extern int add_blocker(struct fd *f, L4_ThreadId_t tid, bool writing);


/* from func.c */

struct io_callbacks
{
	int (*read)(iof_t *, uint8_t *, unsigned, off_t);
	int (*write)(iof_t *, const uint8_t *, unsigned, off_t);
	int (*close)(iof_t *);
	void (*lifecycle)(pid_t, enum lifecycle_tag, ...);
	void (*confirm)(iof_t *, unsigned, off_t, bool);
	int (*ioctl)(iof_t *, long, va_list args);
	int (*stat)(iof_t *, IO_STAT *);

	void *dispatch_priv;
	L4_Word_t (*dispatch)(void *priv);
};

extern struct io_callbacks callbacks;

#endif
