
/* tests on access to initrd data from within systests. */

#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <ccan/minmax/minmax.h>

#include <sneks/test.h>


START_TEST(open_file_and_read)
{
	plan_tests(4);
	todo_start("no functionality");

	int fd = open("/initrd/systest/sys/test/hello.txt", O_RDONLY);
	if(!ok1(fd > 0)) {
		diag("open(2) failed, errno=%d", errno);
	}

	char buffer[200];
	int n = read(fd, buffer, sizeof buffer - 1);
	if(!ok1(n >= 0)) {
		diag("read(2) failed, errno=%d", errno);
	}
	buffer[max(n, 0)] = '\0';
	if(!ok1(strcmp(buffer, "hello, world\n") == 0)) {
		diag("n=%d, buffer=`%s'", n, buffer);
	}

	n = close(fd);
	if(!ok(n == 0, "close(2)")) {
		diag("close(2) failed, errno=%d", errno);
	}
}
END_TEST


SYSTEST("systest:initrd", open_file_and_read);
