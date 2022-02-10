#define BOOTCON_IMPL_SOURCE

#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <ccan/compiler/compiler.h>
#include <l4/types.h>
#include <l4/thread.h>
#include <l4/schedule.h>
#include <l4/ipc.h>
#include <sneks/api/io-defs.h>

#include "muidl.h"
#include "root-impl-defs.h"
#include "defs.h"


static int bootcon_nextfd = 1;


static bool valid(int fd) {
	return fd > 0 && fd < bootcon_nextfd;
}


static int bootcon_read(int fd, int length, off_t offset,
	uint8_t *buf, unsigned *len_p)
{
	if(!valid(fd)) return -EBADF;
	*len_p = 0;
	return 0;
}


static int bootcon_write(int fd, off_t offset,
	const uint8_t *buf, unsigned buf_len)
{
	if(!valid(fd)) return -EBADF;
	extern void computchar(unsigned char ch);
	for(unsigned i=0; i < buf_len; i++) computchar(buf[i]);
	return buf_len;
}


static int bootcon_close(int fd) {
	if(!valid(fd)) return -EBADF;
	return 0;	/* everything will be fire. */
}


static int bootcon_ignore_flags(int *old, int fd, int or, int and) {
	*old = 0;
	if(!valid(fd)) return -EBADF;
	return 0;
}


static int bootcon_dup(int *new_p, int old, int flags) {
	if(!valid(old)) return -EBADF;
	*new_p = bootcon_nextfd++;
	assert(valid(*new_p));
	return 0;
}


static int bootcon_dup_to(int *new_p, int old, pid_t receiver_pid) {
	if(!valid(old)) return -EBADF;
	*new_p = bootcon_nextfd++;
	return 0;
}


static int bootcon_stat_handle(int fd, struct sneks_io_statbuf *st) {
	if(!valid(fd)) return -EBADF;
	*st = (struct sneks_io_statbuf){ };
	return 0;
}


static int bootcon_isatty(int fd) {
	return valid(fd) ? 1 : -EBADF;
}


static int bootcon_thread_fn(void *param_ptr UNUSED)
{
	/* FIXME: make malloc threadsafe and remove this sleep so the dispatch
	 * function can safely enter malloc. or stick the delay in
	 * start_bootcon(), whatever.
	 */
	L4_Sleep(L4_TimePeriod(20 * 1000));

	static const struct boot_con_vtable vtab = {
		.read = &bootcon_read,
		.write = &bootcon_write,
		.close = &bootcon_close,
		.set_file_flags = &bootcon_ignore_flags,
		.set_handle_flags = &bootcon_ignore_flags,
		.dup = &bootcon_dup,
		.dup_to = &bootcon_dup_to,
		.stat_handle = &bootcon_stat_handle,
		.isatty = &bootcon_isatty,
	};
	for(;;) {
		L4_Word_t status = _muidl_boot_con_dispatch(&vtab);
		if(status == MUIDL_UNKNOWN_LABEL) {
			L4_MsgTag_t tag = muidl_get_tag();
			printf("bootcon: unknown message label=%#lx, u=%lu, t=%lu\n",
				L4_Label(tag), L4_UntypedWords(tag), L4_TypedWords(tag));
		} else if(status != 0 && !MUIDL_IS_L4_ERROR(status)) {
			printf("bootcon: dispatch status %#lx (last tag %#lx)\n",
				status, muidl_get_tag().raw);
		}
	}

	return 0;
}


L4_ThreadId_t start_bootcon(int *confd_p, struct htable *root_args)
{
	*confd_p = bootcon_nextfd++;
	thrd_t con_thrd;
	int n = thrd_create(&con_thrd, &bootcon_thread_fn, NULL);
	return n != thrd_success ? L4_nilthread : L4_GlobalIdOf(tidof_NP(con_thrd));
}
