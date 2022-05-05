
/* tests on regular files. */

#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <ccan/str/str.h>

#include <sneks/test.h>


static char testfile_path[] = TESTDIR "/user/test/io/reg/testfile";
static char expected_testfile_data[] = "0123456789abcdef";


START_TEST(open_test_file)
{
	plan_tests(4);

	int fd = open(testfile_path, O_RDONLY);
	skip_start(!ok(fd > 0, "open(2)"), 3, "no file (errno=%d)", errno) {
		char buffer[100];
		memset(buffer, 0, sizeof buffer);
		ssize_t n = read(fd, buffer, sizeof buffer);
		skip_start(!ok(n > 0, "read(2)"), 1, "no data was read") {
			ok1(streq(buffer, expected_testfile_data));
		} skip_end;

		n = close(fd);
		ok(n == 0, "close(2)");
	} skip_end;
}
END_TEST

DECLARE_TEST("io:reg", open_test_file);


START_TEST(rewind_test_file)
{
	plan_tests(9);
	diag("testfile_path=`%s'", testfile_path);

	int fd = open(testfile_path, O_RDONLY);
	skip_start(!ok(fd > 0, "open(2)"), 8, "no file (errno=%d)", errno) {
		char buffer[100];
		memset(buffer, 0, sizeof buffer);
		ssize_t n = read(fd, buffer, sizeof buffer);
		skip_start(!ok(n > 0, "read(2)"), 6, "no data was read") {
			ok1(streq(buffer, expected_testfile_data));

			off_t prev = lseek(fd, 0, SEEK_CUR);
			if(!ok(prev >= 0, "lseek(2) query")) diag("errno=%d", errno);
			ok1(prev == strlen(buffer));
			off_t cur = lseek(fd, 0, SEEK_SET);
			if(!ok(cur == 0, "lseek(2) rewind")) diag("errno=%d", errno);

			memset(buffer, 0, sizeof buffer);
			n = read(fd, buffer, sizeof buffer);
			ok(n > 0, "read(2) after rewind");
			ok1(streq(buffer, expected_testfile_data));
		} skip_end;

		n = close(fd);
		ok(n == 0, "close(2)");
	} skip_end;
}
END_TEST

DECLARE_TEST("io:reg", rewind_test_file);


START_TEST(open_and_seek_test_file)
{
	plan_tests(5);

	int fd = open(testfile_path, O_RDONLY);
	skip_start(!ok(fd > 0, "open(2)"), 4, "no file (errno=%d)", errno) {
		off_t cur = lseek(fd, 10, SEEK_SET);
		if(!ok(cur == 10, "lseek(2)")) diag("cur=%ld, errno=%d", (long)cur, errno);

		char buffer[100];
		memset(buffer, 0, sizeof buffer);
		ssize_t n = read(fd, buffer, sizeof buffer);
		skip_start(!ok(n > 0, "read(2)"), 1, "no data was read") {
			if(!ok1(streq(buffer, expected_testfile_data + 10))) {
				diag("buffer=`%s', expected=`%s'", buffer,
					expected_testfile_data + 10);
			}
		} skip_end;

		n = close(fd);
		ok(n == 0, "close(2)");
	} skip_end;
}
END_TEST

DECLARE_TEST("io:reg", open_and_seek_test_file);


/* the "poor man's fstat()", common in portable programs. */
START_TEST(seek_for_length)
{
	plan_tests(3);

	int fd = open(testfile_path, O_RDONLY);
	skip_start(!ok(fd > 0, "open(2)"), 2, "no file (errno=%d)", errno) {
		off_t cur = lseek(fd, 0, SEEK_END);
		if(!ok1(cur == strlen(expected_testfile_data))) {
			diag("cur=%d, errno=%d", (int)cur, errno);
		}

		int n = close(fd);
		ok(n == 0, "close(2)");
	} skip_end;
}
END_TEST

DECLARE_TEST("io:reg", seek_for_length);
