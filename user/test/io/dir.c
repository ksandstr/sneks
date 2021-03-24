
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <alloca.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <sys/types.h>
#include <ccan/str/str.h>
#include <ccan/darray/darray.h>

#include <sneks/test.h>


static const char test_directory[] = TESTDIR "/user/test/io/dir";


static void validate_dirent(const struct dirent *ent, int count)
{
	assert(ent != NULL);
	subtest_start("%s: count=%d", __func__, count);
	diag("ent->d_name=`%s'", ent->d_name);
	plan_tests(4);

	ok1(strlen(ent->d_name) > 0);
	skip_start(!ok1(ent->d_type == DT_REG || ent->d_type == DT_DIR), 2, "not reg or dir") {
		char fullpath[NAME_MAX + 100];
		snprintf(fullpath, sizeof fullpath, "%s/%s", test_directory, ent->d_name);
		int dir = open(fullpath, O_DIRECTORY | O_RDONLY);
		if(dir < 0) diag("errno=%d", errno);
		imply_ok1(ent->d_type == DT_DIR, dir >= 0);
		imply_ok1(ent->d_type == DT_REG, dir < 0 && errno == ENOTDIR);
		if(dir >= 0) close(dir);
	} skip_end;

	subtest_end();
}


START_LOOP_TEST(basic_opendir, iter, 0, 1)
{
	const bool posix_open = !!(iter & 1);
	diag("posix_open=%s", btos(posix_open));
	plan_tests(3 + 22);

#ifdef __sneks__
	todo_start("impl missing");
#endif

	DIR *dirp;
	if(!posix_open) dirp = opendir(test_directory);
	else {
		int fd = open(test_directory, O_DIRECTORY);
		dirp = fd < 0 ? NULL : fdopendir(fd);
	}
	skip_start(!ok(dirp != NULL, "dir opened"), 24, "no dir, errno=%d", errno) {
		struct dirent *ent;
		int total = 0;
		while(errno = 0, ent = readdir(dirp), ent != NULL) {
			if(strstarts(ent->d_name, ".")) continue;
			validate_dirent(ent, total++);
		}
		if(total < 22) skip(22 - total, "total less than 22");
		if(!ok(errno == 0, "no error")) diag("errno=%d", errno);
		ok1(closedir(dirp) == 0);
	} skip_end;
}
END_TEST

DECLARE_TEST("io:dir", basic_opendir);


/* record dirent offsets from readdir() or telldir(), and seekdir() to them
 * afterward.
 *
 * counterintuitively, dirent.d_off is the position where readdir() leaves the
 * directory handle rather than the position of the data that was returned.
 * this field is also not found in the POSIX spec for <struct dirent>; that
 * one only has d_ino and d_name (which is at most NAME_MAX+1 bytes long).
 */
struct entry {
	ino_t ino;
	off_t offset;
	char *name;
};


START_LOOP_TEST(dirent_offsets, iter, 0, 1)
{
	const bool use_telldir = !!(iter & 1);
	diag("use_telldir=%s", btos(use_telldir));
#ifndef _DIRENT_HAVE_D_OFF
	if(!use_telldir) {
		plan_skip_all("target system <struct dirent> has no `d_off'");
		return;
	}
#endif
	plan_tests(7);

#ifdef __sneks__
	todo_start("impl missing");
#endif

	DIR *dirp = opendir(test_directory);
	skip_start(!ok(dirp != NULL, "opendir(3)"), 6, "no dir, errno=%d", errno) {
		darray(struct entry) ents = darray_new();
#ifdef __sneks__
		ok(telldir(dirp) == 0, "starts at 0");
		off_t prev = 0;
#else
		skip(1, "starting telldir(3) is opaque");
		off_t prev = telldir(dirp);
#endif
		struct dirent *d;
		while(errno = 0, d = readdir(dirp), d != NULL) {
			struct entry e = { .ino = d->d_ino, .offset = prev };
			e.name = alloca(strlen(d->d_name) + 1);
			memcpy(e.name, d->d_name, strlen(d->d_name) + 1);
			darray_push(ents, e);

#ifndef _DIRENT_HAVE_D_OFF
			prev = telldir(dirp);
#else
			prev = use_telldir ? telldir(dirp) : d->d_off;
#endif
		}
		if(!ok(d == NULL && errno == 0, "first read loop")) {
			diag("errno=%d", errno);
		}
		ok1(ents.size > 10);

		bool all_match = true, no_nulls = true;
		struct entry *ep;
		darray_foreach(ep, ents) {
			seekdir(dirp, ep->offset);
			errno = 0;
			d = readdir(dirp);
			if(d == NULL) {
				if(errno != 0) diag("readdir(3) errno=%d", errno);
				no_nulls = false;
			} else if(!streq(d->d_name, ep->name) || d->d_ino != ep->ino) {
				diag("ep->name=`%s', ->ino=%d, but d->d_name=`%s', ->d_ino=%d",
					ep->name, (int)ep->ino, d->d_name, (int)d->d_ino);
				all_match = false;
			}
		}
		ok1(no_nulls);
		ok1(all_match);

		int n = closedir(dirp);
		ok(n == 0, "closedir(3)");

		darray_free(ents);
	} skip_end;
}
END_TEST

DECLARE_TEST("io:dir", dirent_offsets);


/* very basic function of scandir. covers the positive case where no errors
 * occur and there are entries in the directory given.
 */

static int reverse_alphasort(const struct dirent **, const struct dirent **);
static int include_even(const struct dirent *);

START_TEST(scandir_basic)
{
	plan_tests(3);

#ifdef __sneks__
	todo_start("impl missing");
#endif

	errno = 0;
	struct dirent **even_only;
	int n = scandir(test_directory, &even_only, &include_even,
		&reverse_alphasort);
	skip_start(!ok1(n > 10), 2, "scandir(3) failed, errno=%d", errno) {
		bool all_even = true, reversed = true;
		for(int i=0; i < n; i++) {
			if(even_only[i]->d_name[0] & 1) {
				all_even = false;
				diag("first letter of `%s' is odd", even_only[i]->d_name);
			}
			if(i > 1 && strcmp(even_only[i - 1]->d_name, even_only[i]->d_name) < 0) {
				reversed = false;
				diag("`%s' and `%s' are not in descending order",
					even_only[i - 1]->d_name, even_only[i]->d_name);
			}
		}

		ok1(all_even);
		ok1(reversed);

		while(n--) free(even_only[n]);
		free(even_only);
	} skip_end;
}
END_TEST

DECLARE_TEST("io:dir", scandir_basic);


static int reverse_alphasort(const struct dirent **a, const struct dirent **b) {
	return strcmp((*b)->d_name, (*a)->d_name);
}

static int include_even(const struct dirent *a) {
	return !strstarts(a->d_name, ".") && !(a->d_name[0] & 1);
}
