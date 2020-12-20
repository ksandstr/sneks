
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <ccan/str/str.h>

#include <sneks/test.h>


/* tests on /dev/{null,full,zero} */
START_LOOP_TEST(sinkdev, iter, 0, 2)
{
	static const char *dev_names[] = { "null", "full", "zero" };
	char dev_path[16];
	snprintf(dev_path, sizeof dev_path, "/dev/%s", dev_names[iter]);
	const bool is_null = streq(dev_path, "/dev/null"),
		is_sink = !streq(dev_path, "/dev/full");
	diag("dev_path=`%s', is_null=%s, is_sink=%s",
		dev_path, btos(is_null), btos(is_sink));
	plan(7);

#ifdef __sneks__
	todo_start("impl missing");
#endif

	int fd = open(dev_path, O_RDWR);
	if(fd < 0) diag("fd=%d, errno=%d", fd, errno);
	skip_start(!ok(fd > 0, "open device"), 6, "no fd") {
		/* TODO: fstat it, check that it's a character device */

		ssize_t n_written = write(fd, "foo", 4);
		imply_ok1(is_sink, n_written == 4);
		imply_ok1(!is_sink, n_written < 0 && errno == ENOSPC);

		uint8_t buf[12];
		memset(buf, 0x7f, sizeof buf);
		ssize_t n_read = read(fd, buf, sizeof buf);
		imply_ok1(is_null, n_read == 0);
		imply_ok1(!is_null, n_read == sizeof buf);
		skip_start(is_null, 1, "not applicable") {
			bool is_zeroes = true;
			for(int i=0; i < sizeof buf; i++) {
				if(buf[i] != '\0') {
					is_zeroes = false;
					diag("buf[%d]=%#02x", i, buf[i]);
				}
			}
			ok1(is_zeroes);
		} skip_end;

		int n = close(fd);
		if(!ok(n == 0, "close")) diag("errno=%d", errno);
	} skip_end;
}
END_TEST

DECLARE_TEST("io:chrdev", sinkdev);
