#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <ccan/array_size/array_size.h>
#include <sneks/chrdev.h>

enum null_dev { DEV_NULL = 1, DEV_ZERO, DEV_FULL };

struct chrdev_file_impl {
	enum null_dev dev;
};

static int null_get_status(chrfile_t *h)
{
	static const int st[] = {
		[DEV_NULL] = EPOLLOUT,
		[DEV_ZERO] = EPOLLIN | EPOLLOUT,
		[DEV_FULL] = EPOLLIN | EPOLLOUT,
	};
	assert(h->dev >= 0 && h->dev < ARRAY_SIZE(st));
	return st[h->dev];
}

static int null_read(chrfile_t *h, uint8_t *buf, unsigned count, off_t offset)
{
	switch(h->dev) {
		case DEV_NULL: return 0;
		case DEV_FULL:
		case DEV_ZERO:
			memset(buf, '\0', count);
			return count;
		default: return -ENOSYS; /* not reached */
	}
}

static int null_write(chrfile_t *h, const uint8_t *buf, unsigned buf_len, off_t offset)
{
	switch(h->dev) {
		case DEV_FULL: return -ENOSPC;
		case DEV_NULL:
		case DEV_ZERO:
			return buf_len;
		default: return -ENOSYS;	/* not reached */
	}
}

static int null_close(chrfile_t *h) { return 0; }

/* recognize type='c', major=1 minors=(3, 5, 7) as null, zero, full
 * respectively.
 */
static int null_open(chrfile_t *h, char type, int major, int minor, int flags)
{
	static const enum null_dev ds[] = { [3] = DEV_NULL, [5] = DEV_ZERO, [7] = DEV_FULL };
	if(type == 'c' && major == 1 && minor >= 0 && minor < ARRAY_SIZE(ds) && ds[minor] != 0) {
		h->dev = ds[minor];
		return 0;
	} else {
		return -ENODEV;
	}
}

int main(int argc, char *argv[])
{
	chrdev_get_status_func(&null_get_status);
	chrdev_read_func(&null_read);
	chrdev_write_func(&null_write);
	chrdev_close_func(&null_close);
	chrdev_dev_open_func(&null_open);
	return chrdev_run(sizeof(struct chrdev_file_impl), argc, argv);
}
