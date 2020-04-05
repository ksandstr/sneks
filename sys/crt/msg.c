
/* sysmsg client-side implementation. */

#include <errno.h>
#include <sneks/msg.h>


int sysmsg_listen(int bit, sysmsg_handler_fn fn, void *priv)
{
	return -ENOSYS;
}


int sysmsg_add_filter(int handle, const L4_Word_t *labels, int n_labels)
{
	/* no-op */
	return 0;
}


int sysmsg_rm_filter(int handle, const L4_Word_t *labels, int n_labels)
{
	/* also a no-op */
	return 0;
}


int sysmsg_broadcast(int maskp, int maskn, const L4_Word_t *body, int length)
{
	return -ENOSYS;
}


int sysmsg_close(int handle)
{
	return -EBADF;
}
