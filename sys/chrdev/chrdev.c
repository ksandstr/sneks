
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>
#include <sneks/chrdev.h>


static int enosys();
static void no_confirm(chrfile_t *, unsigned, bool);


static struct {
	int (*get_status)(chrfile_t *);
	int (*dead_client)(pid_t);
	int (*read)(chrfile_t *, uint8_t *, unsigned);
	int (*write)(chrfile_t *, const uint8_t *, unsigned);
	void (*confirm)(chrfile_t *, unsigned, bool);
	int (*close)(chrfile_t *);
	int (*ioctl)(chrfile_t *, unsigned long, va_list args);
	int (*fork)(chrfile_t *, chrfile_t *);
	int (*pipe)(chrfile_t *, chrfile_t *, int);
} callbacks = {
	.get_status = &enosys,
	.dead_client = &enosys,
	.read = &enosys, .write = &enosys,
	.confirm = &no_confirm,
	.close = &enosys,
	.ioctl = &enosys,
	.fork = &enosys,
	.pipe = &enosys,
};


static int enosys() {
	return -ENOSYS;
}


static void no_confirm(chrfile_t *foo, unsigned bar, bool zot) {
	/* does nothing */
}


void chrdev_notify(chrfile_t *file, int mask) {
	/* stub */
}


void chrdev_get_status_func(int (*fn)(chrfile_t *)) {
	callbacks.get_status = fn;
}


void chrdev_dead_client_func(int (*fn)(pid_t)) {
	callbacks.dead_client = fn;
}


void chrdev_read_func(int (*fn)(chrfile_t *, uint8_t *, unsigned)) {
	callbacks.read = fn;
}


void chrdev_write_func(int (*fn)(chrfile_t *, const uint8_t *, unsigned)) {
	callbacks.write = fn;
}


void chrdev_confirm_func(void (*fn)(chrfile_t *, unsigned, bool)) {
	callbacks.confirm = fn;
}


void chrdev_close_func(int (*fn)(chrfile_t *)) {
	callbacks.close = fn;
}


void chrdev_ioctl_func(int (*fn)(chrfile_t *, unsigned long, va_list)) {
	callbacks.ioctl = fn;
}


void chrdev_fork_func(int (*fn)(chrfile_t *, chrfile_t *)) {
	callbacks.fork = fn;
}


void chrdev_pipe_func(int (*fn)(chrfile_t *, chrfile_t *, int)) {
	callbacks.pipe = fn;
}


int chrdev_run(size_t sizeof_file, int argc, char *argv[])
{
	/* stub IPC loop */
	for(;;) {
		L4_ThreadId_t sender;
		L4_MsgTag_t tag = L4_Wait(&sender);
		for(;;) {
			if(L4_IpcFailed(tag)) {
				L4_Word_t ec = L4_ErrorCode();
				fprintf(stderr, "chrdev[%d]: stub IPC failure, ec=%#lx\n",
					getpid(), ec);
				break;
			}

			L4_LoadMR(0, (L4_MsgTag_t){ .X.label = 1, .X.u = 1 }.raw);
			L4_LoadMR(1, ENOSYS);
			tag = L4_ReplyWait(sender, &sender);
		}
	}
}
