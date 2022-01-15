/* tests on C11-style file I/O. fopen(), fprintf() and the like, into streams
 * of various kinds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <ccan/str/str.h>
#include <sneks/test.h>


static void chomp(char *s) {
	int len = strlen(s);
	while(len > 0 && s[--len] == '\n') s[len] = '\0';
}


/* scratch test: whether a stream opened read-write retains what's written at
 * predictable positions.
 */
static int sub_scratch_test(FILE *stream)
{
	subtest_start("scratch test");

	plan_tests(3);

	rewind(stream);
	fprintf(stream, "hello, sneks unit test suite!\n");
	fflush(stream);

	rewind(stream);
	char c = fgetc(stream);
	if(!ok1(c == 'h')) diag("c=%#x", (unsigned int)c);

	rewind(stream);
	char s[64], *sp = fgets(s, sizeof s, stream);
	ok(sp == s, "fgets(3)");
	ok1(memcmp(s, "hello, sneks", 10) == 0);

	return subtest_end();
}


/* seek-tell test: whether seeking affects the result of ftell(). */
static int sub_seek_tell_test(FILE *stream)
{
	subtest_start("seek-tell test");

	plan_tests(12);

	ok1(fseek(stream, 0, SEEK_SET) == 0);
	ok1(ftell(stream) == 0);
	ok1(fseek(stream, 0, SEEK_END) == 0);
	ok1(ftell(stream) > 0);
	long end = ftell(stream);
	diag("end=%ld", end);

	ok1(fseek(stream, -(end / 3), SEEK_END) == 0);
	ok1(ftell(stream) == end - (end / 3));
	ok1(fseek(stream, end / 3, SEEK_SET) == 0);
	ok1(ftell(stream) == end / 3);
	ok1(fseek(stream, end / 3, SEEK_CUR) == 0);
	ok1(ftell(stream) == (end / 3) * 2);

	/* positive offsets to SEEK_END shouldn't work. */
	ok1(fseek(stream, end / 3, SEEK_END) < 0 && errno == EINVAL);
	/* nor negative ones to SEEK_SET. */
	ok1(fseek(stream, -(end / 3), SEEK_SET) < 0 && errno == EINVAL);

	return subtest_end();
}


/* basic interface tests on memory streams, as from fmemopen(). */
START_LOOP_TEST(fmemopen_basic, iter, 0, 1)
{
	const bool anon = !!(iter & 1);
	diag("anon=%s", btos(anon));
	plan_tests(4);

	const int bufsz = 16 * 1024;
	void *ptr = anon ? NULL : malloc(bufsz);
	fail_if(!anon && ptr == NULL);

	FILE *memf = fmemopen(ptr, bufsz, "r+");
	skip_start(!ok1(memf != NULL), 2, "no memf") {
		sub_seek_tell_test(memf);
		sub_scratch_test(memf);
	} skip_end;

	ok1(fclose(memf) == 0);
	free(ptr);
}
END_TEST

DECLARE_TEST("cstd:fileio", fmemopen_basic);


/* data retrieval from a non-anonymous memory stream. */
START_TEST(fmemopen_buffer_write)
{
	plan_tests(2);

	const size_t bufsz = 16 * 1024;
	void *buf = malloc(bufsz);
	fail_if(buf == NULL);
	FILE *memf = fmemopen(buf, bufsz, "w");
	fail_unless(memf != NULL);

	fprintf(memf, "hello, world!");
	fflush(memf);
	ok1(strcmp(buf, "hello, world!") == 0);

	ok1(fclose(memf) == 0);
	free(buf);
}
END_TEST

DECLARE_TEST("cstd:fileio", fmemopen_buffer_write);


/* reading of data from a designated buffer. */
START_TEST(fmemopen_buffer_read)
{
	plan_tests(3);

	const char *input = "hello, world!\n";
	void *buf = strdup(input);
	fail_if(buf == NULL);
	FILE *memf = fmemopen(buf, strlen(buf) + 1, "r");
	fail_unless(memf != NULL);

	char line[100];
	line[0] = '\0';
	ok1(fgets(line, sizeof line, memf));
	ok1(strcmp(line, "hello, world!\n") == 0);

	ok1(fclose(memf) == 0);
	free(buf);
}
END_TEST

DECLARE_TEST("cstd:fileio", fmemopen_buffer_read);


/* per this function, a zero-length buffer will not allow reads. similar for
 * allows_write().
 */
static bool allows_read(FILE *stream)
{
	long pos = ftell(stream);
	rewind(stream);
	int c = fgetc(stream);
	fseek(stream, pos, SEEK_SET);
	return c != EOF;
}


/* this necessarily alters stream contents, so we'll do it at the very end. */
static bool allows_write(FILE *stream)
{
	long pos = ftell(stream);
	fseek(stream, -1, SEEK_END);
	int n = fputc('@', stream);
	fseek(stream, pos, SEEK_SET);
	return n != EOF;
}


START_LOOP_TEST(fmemopen_mode, iter, 0, 5)
{
	const bool mode_plus = !!(iter & 1);
	const char mode_rwa = "rwa"[iter >> 1];
	diag("mode_plus=%s, mode_rwa=%c", btos(mode_plus), mode_rwa);

	char modestr[8];
	snprintf(modestr, sizeof modestr, "%c%s", mode_rwa, mode_plus ? "+" : "");
	diag("modestr=`%s'", modestr);

	plan_tests(5);

	const size_t bufsz = 16 * 1024;
	char *buffer = malloc(bufsz);
	fail_if(buffer == NULL);
	strscpy(buffer, "all work and no play makes jack a dull boy", bufsz);
	FILE *stream = fmemopen(buffer, bufsz, modestr);
	skip_start(!ok1(stream != NULL), 4, "no stream") {
		long pos = ftell(stream);
		diag("pos=%ld, buffer=`%s'", pos, buffer);

		iff_ok1(mode_rwa == 'a', pos > 0);	/* seeking */
		iff_ok1(mode_rwa == 'a' || streq(modestr, "w+"),
			buffer[pos] == '\0');	/* seeking or truncation */
		iff_ok1(mode_rwa == 'r' || mode_plus, allows_read(stream));
		iff_ok1(mode_rwa != 'r' || mode_plus, allows_write(stream));
	} skip_end;

	fclose(stream);
	free(buffer);
}
END_TEST

DECLARE_TEST("cstd:fileio", fmemopen_mode);


/* what happens when EOF occurs in the middle of a fread() element? this
 * question is mildly hairy because fopencookie()'s interface doesn't pass
 * fread's size parameter, and some streams cannot be rewound to back down to
 * a multiple of size, so there must be a defined behaviour that leaves the
 * file at EOF afterward.
 */
START_LOOP_TEST(fread_odd_block, iter, 0, 1)
{
	const int bufsz = 16 * 1024,
		chunksz = bufsz / (!!(iter & 1) ? 61 : 383);
	diag("bufsz=%d, chunksz=%d (rem=%d)", bufsz, chunksz,
		bufsz % chunksz);
	plan_tests(4);

	void *buf = malloc(bufsz);
	memset(buf, 0xff, bufsz);
	FILE *stream = fmemopen(buf, bufsz, "r+");

	uint8_t *result = malloc(bufsz * 2);
	memset(result, 0xda, bufsz * 2);
	int n = fread(result, chunksz, bufsz / chunksz + 1, stream);
	ok1(n == bufsz / chunksz);
	// ok1(feof(stream));
	ok1(ftell(stream) == bufsz);
	ok1(memcmp(buf, result, bufsz) == 0);

	bool tail_unaltered = true;
	for(int i=bufsz; i < (bufsz / chunksz + 1) * chunksz; i++) {
		tail_unaltered = tail_unaltered & (result[i] == 0xda);
	}
	ok1(tail_unaltered);

	fclose(stream);
	free(result);
	free(buf);
}
END_TEST

DECLARE_TEST("cstd:fileio", fread_odd_block);


/* just open a file and read its contents. */
START_TEST(fopen_basic)
{
	plan_tests(3);
	FILE *f = fopen(TESTDIR "/user/test/cstd/fileio/test-file", "r");
	skip_start(!ok(f != NULL, "fopen(3)"), 2, "errno=%d", errno) {
		char line[200] = "";
		ok(fgets(line, sizeof line, f) != NULL, "fgets(3)");
		chomp(line);
		ok1(streq(line, "hello, test file"));
		fclose(f);
	} skip_end;
}
END_TEST

DECLARE_TEST("cstd:fileio", fopen_basic);
