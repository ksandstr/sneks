/* tests on mount(2) and umount2(2). specific to sneks.
 *
 * NOTE: these tests will eventually break because they won't have privilege
 * to mount things anywhere. at that point it should be fixed to also work in
 * hostcheck.
 */
#ifdef __sneks__
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <ccan/str/str.h>
#include <sneks/test.h>

const char *mountpoint = TESTDIR "/user/test/path/mount/sub",
	*image = TESTDIR "/user/test/path/mount/subfs.img",
	*notmount = TESTDIR "/user/test/path/mount/sub/not-mounted-yet";

/* mount a squashfs image from the initrd within a test directory, and read a
 * file within. test some properties of file visibility through a mount-umount
 * cycle.
 */
START_TEST(mount_basic)
{
	diag("mountpoint=`%s', image=`%s'", mountpoint, image);
	plan_tests(9);

	struct stat st;
	fail_unless(test_e(mountpoint));
	fail_unless(test_e(image));

	/* base case: the file isn't accessible before mounting. */
	char testpath[80];
	snprintf(testpath, sizeof testpath, "%s/test-file", mountpoint);
	ok(stat(testpath, &st) < 0 && errno == ENOENT, "no testfile before mount");
	/* and files within the mountpoint directory are. */
	ok(test_e(notmount), "pre-mount file seen");

	todo_start("unfinished");
	int n = mount(image, mountpoint, "squashfs", 0, NULL);
	skip_start(!ok(n == 0, "mount"), 6, "didn't mount: %s", strerror(errno)) {
		ok(stat(notmount, &st) < 0 && errno == ENOENT, "pre-mount file not seen");
		FILE *f = fopen(testpath, "r");
		if(!ok(f != NULL, "open testpath")) {
			diag("errno=%d, testpath=`%s'", errno, testpath);
			skip(1, "no testfile");
		} else {
			char line[100] = "";
			fgets(line, sizeof line, f);
			ok1(streq(line, "hello, test mount!\n"));
			fclose(f);
		}
		n = umount(mountpoint);
		if(!ok(n == 0, "umount")) diag("errno=%d", errno);
		ok(stat(testpath, &st) < 0 && errno == ENOENT, "testpath gone");
		ok(test_e(notmount), "pre-mount file seen");
	} skip_end;
}
END_TEST

DECLARE_TEST("path:mount", mount_basic);

/* mount a filesystem, then umount it, a few times. do other things to it in
 * between, or don't.
 */
START_LOOP_TEST(mount_umount_serial, iter, 0, 5)
{
	const int cycles = (iter & 1) ? 7 : 2;
	const bool do_test = (iter >> 1) == 1, do_open = (iter >> 1) == 2;
	diag("cycles=%d, do_test=%s, do_open=%s", cycles, btos(do_test), btos(do_open));
	plan_tests(5);

	char testpath[100]; snprintf(testpath, sizeof testpath, "%s/test-file", mountpoint);
	bool mount_ok = true, umount_ok = true, test_ok = true, open_ok = true;
	int last_i = -1;
	for(int i=0; i < cycles; i++) {
		diag("i=%d ...", i);
		int n = mount(image, mountpoint, "squashfs", 0, NULL);
		if(n < 0) {
			mount_ok = false;
			diag("i=%d, mount failed, errno=%d", i, errno);
			break;
		}
		if(do_test && !test_e(testpath)) {
			test_ok = false;
			diag("i=%d, 'test -e' failed, errno=%d", i, errno);
		}
		if(do_open) {
			FILE *f = fopen(testpath, "r");
			if(f == NULL) {
				open_ok = false;
				diag("i=%d, fopen(testpath, ...) failed, errno=%d", i, errno);
			} else {
				fclose(f);
			}
		}
		n = umount(mountpoint);
		if(n < 0) {
			umount_ok = false;
			diag("i=%d, umount failed, errno=%d", i, errno);
		}
		last_i = i;
	}

	todo_start("breakage expected");
	ok1(mount_ok);
	ok1(umount_ok);
	ok1(test_ok);
	ok1(open_ok);
	ok(last_i == cycles - 1, "completed");
}
END_TEST

DECLARE_TEST("path:mount", mount_umount_serial);

/* test that umount returns EBUSY when descriptors are open, and EINVAL when
 * the mountpoint already doesn't have a filesystem.
 */
START_LOOP_TEST(umount_busy_status, iter, 0, 1)
{
	char testpath[100]; snprintf(testpath, sizeof testpath, "%s/test-file", mountpoint);
	diag("mountpoint=`%s', image=`%s'", mountpoint, image);
	const bool file_open = iter & 1;
	diag("file_open=%s", btos(file_open));
	plan_tests(8);

	todo_start("unimplemented");
	int n = mount(image, mountpoint, "squashfs", 0, NULL);
	skip_start(!ok(n == 0, "mount"), 6, "didn't mount, errno=%d", errno) {
		FILE *f = NULL;
		skip_start(!file_open, 1, "won't open file") {
			f = fopen(testpath, "r");
			ok(f != NULL, "open testpath");
		} skip_end;
		skip_start(file_open, 1, "will open file") {
			ok1(test_e(testpath));
		} skip_end;
		n = umount(mountpoint);
		diag("first umount n=%d, errno=%d", n, errno);
		imply_ok1(file_open, n < 0 && errno == EBUSY);
		imply_ok1(!file_open, n == 0);
		if(file_open && f != NULL) fclose(f);
		n = umount(mountpoint);
		diag("second umount n=%d, errno=%d", n, errno);
		imply_ok1(file_open, n == 0);
		imply_ok1(!file_open, n < 0 && errno == EINVAL);
	} skip_end;
	ok1(!test_e(testpath));
}
END_TEST

DECLARE_TEST("path:mount", umount_busy_status);

/* test path resolution that falls out of a subfilesystem.
 * variables:
 *   - [sub_dir] resolve wrt subfilesystem root, or a directory therein.
 *   - [back_in] resolution should fall back into the subfilesystem.
 */
START_LOOP_TEST(resolve_fallout, iter, 0, 3)
{
	const bool sub_dir = iter & 1, back_in = iter & 2;
	diag("sub_dir=%s, back_in=%s", btos(sub_dir), btos(back_in));
	plan_tests(5);

	todo_start("expected to fail");
	int n = mount(image, mountpoint, "squashfs", 0, NULL);
	skip_start(!ok(n == 0, "mount"), 4, "didn't mount, errno=%d", errno) {
		char test_file[strlen(mountpoint) + 16];
		snprintf(test_file, sizeof test_file, "%s/test-file", mountpoint);
		if(!ok1(test_e(test_file))) diag("test_file=`%s'", test_file);
		int dirfd;
		if(!sub_dir) {
			diag("opening mountpoint=`%s' for dirfd", mountpoint);
			dirfd = open(mountpoint, O_DIRECTORY);
		} else {
			char dirdir[strlen(mountpoint) + 16];
			snprintf(dirdir, sizeof dirdir, "%s/directory", mountpoint);
			diag("opening dirdir=`%s' for dirfd", dirdir);
			dirfd = open(dirdir, O_DIRECTORY);
		}
		skip_start(!ok(dirfd >= 0, "dir open"), 1, "dirfd didn't open, errno=%d", errno) {
			const char *out = sub_dir ? "../.." : "..", *in = back_in ? "sub/" : "", *file = back_in ? "test-file" : "super-test-file";
			char path[100];
			snprintf(path, sizeof path, "%s/%s%s", out, in, file);
			diag("path=`%s'", path);
			int fd = openat(dirfd, path, 0);
			if(!ok(fd >= 0, "openat(dirfd, path, 0)")) diag("errno=%d", errno);
			else close(fd);
		} skip_end;
		close(dirfd);
		ok1(umount(mountpoint) == 0);
	} skip_end;
}
END_TEST

DECLARE_TEST("path:mount", resolve_fallout);
#endif
