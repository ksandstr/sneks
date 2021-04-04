
#ifdef __sneks__

/* tests on rollbacks of failed Sneks::Directory/{seekdir,getdents} calls.
 * the general theme is that the previous position should be retained on
 * failure.
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <sys/types.h>
#include <ccan/str/str.h>
#include <ccan/talloc/talloc.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>
#include <l4/message.h>

#include <sneks/api/io-defs.h>
#include <sneks/api/directory-defs.h>
#include <sneks/crtprivate.h>
#include <sneks/test.h>


static const char test_directory[] = TESTDIR "/user/test/io/dir";


static struct sneks_directory_dentry *copy_dentry(
	TALLOC_CTX *ctx, const struct sneks_directory_dentry *orig)
{
	assert(orig->reclen >= sizeof *orig + strlen((char *)&orig[1]) + 1);
	return talloc_memdup(ctx, orig, orig->reclen);
}


static const char *dentry_name(const struct sneks_directory_dentry *dent) {
	return (const char *)&dent[1];
}


/* moves @dir forward by as many entries as the server deigns to return. sets
 * *@offset_p to the new position.
 */
static struct sneks_directory_dentry *get_first_dentry(
	TALLOC_CTX *ctx, const struct fd_bits *bits, off_t *offset_p)
{
	uint8_t buf[SNEKS_DIRECTORY_DENTSBUF_MAX];
	memset(buf, 0, sizeof buf);
	unsigned buf_fill = sizeof buf;
	uint16_t count;
	int n = __dir_getdents(bits->server, &count, bits->handle,
		&(off_t){ offset_p != NULL ? *offset_p : -1 },
		offset_p != NULL ? offset_p : &(off_t){ 0 }, buf, &buf_fill);
	if(n != 0) {
		diag("%s: Directory::getdents failed, n=%d", __func__, n);
		return NULL;
	} else if(count == 0 || buf_fill < sizeof(struct sneks_directory_dentry) + 2) {
		/* EOD */
		return NULL;
	} else {
		return copy_dentry(ctx, (void *)buf);
	}
}


/* test that Sneks::Directory/seekdir does not alter the directory position on
 * IPC rollback.
 */
START_TEST(rollback_seekdir)
{
	plan_tests(6);
	TALLOC_CTX *tal = talloc_new(NULL);

	DIR *dir = opendir(test_directory);
	assert(dir != NULL);
	struct fd_bits *bits = __fdbits(dirfd(dir));
	assert(bits != NULL);

	int init_error = 0;
	off_t startpos = 0, testpos = 0;
	for(int i=0; i < 7; i++) {
		errno = 0;
		struct dirent *dent = readdir(dir);
		if(dent == NULL) {
			init_error = errno;
			break;
		}
		if(i == 3) startpos = dent->d_off;
		else if(i == 6) testpos = dent->d_off;
	}
	if(!ok1(init_error == 0)) {
		diag("readdir() error %d", init_error);
	}
	diag("startpos=%d, testpos=%d", startpos, testpos);

	/* gather reference data */
	int n = __dir_seekdir(bits->server, bits->handle, &(off_t){ startpos });
	if(!ok(n == 0, "ref seekdir")) diag("n=%d", n);
	struct sneks_directory_dentry *ref = get_first_dentry(tal, bits, NULL);
	ok(ref != NULL, "ref dentry");

	/* perform actual test */
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 3, .X.label = 0xe80a }.raw);
	L4_LoadMR(1, 0xb336);	/* sublabel per api/directory.idl */
	L4_LoadMR(2, bits->handle);
	L4_LoadMR(3, testpos);
	L4_MsgTag_t tag = L4_Send_Timeout(bits->server, L4_TimePeriod(20 * 1000));
	L4_Word_t ec = L4_ErrorCode();
	if(!ok(L4_IpcSucceeded(tag), "seekdir send")) diag("ec=%lu", ec);

	/* examine entrails. */
	struct sneks_directory_dentry *dat = get_first_dentry(tal, bits, NULL);
	skip_start(!ok(dat != NULL, "test dentry"), 1, "no test data") {
		if(!ok1(streq(dentry_name(ref), dentry_name(dat)))) {
			diag("ref name=`%s', dat name=`%s'", dentry_name(ref),
				dentry_name(dat));
		}
	} skip_end;

	closedir(dir);
	talloc_free(tal);
}
END_TEST

DECLARE_TEST("io:dir", rollback_seekdir);


/* test that Sneks::Directory/getdents does not alter the directory position
 * on IPC rollback.
 *
 * variables:
 *   - [go_forward] read a few entries down the directory before testing
 */
START_LOOP_TEST(rollback_getdents, iter, 0, 1)
{
	const bool go_forward = !!(iter & 1);
	diag("go_forward=%s", btos(go_forward));
	plan_tests(7);
	TALLOC_CTX *tal = talloc_new(NULL);

	DIR *dir = opendir(test_directory);
	assert(dir != NULL);
	struct fd_bits *bits = __fdbits(dirfd(dir));
	assert(bits != NULL);

	off_t seekpos = -1;
	if(go_forward) {
		for(int i=0; i < 3; i++) {
			struct dirent *dent = readdir(dir);
			if(dent != NULL) seekpos = dent->d_off;
		}
	} else {
		seekpos = 0;
	}
	if(!ok(seekpos >= 0, "valid seekpos")) diag("seekpos=%d", seekpos);

	/* gather reference data */
	int n = __dir_seekdir(bits->server, bits->handle, &(off_t){ seekpos });
	if(!ok(n == 0, "ref seekdir")) diag("n=%d", n);
	struct sneks_directory_dentry *ref = get_first_dentry(tal, bits, NULL);
	ok(ref != NULL, "ref dentry");

	/* perform actual test */
	n = __dir_seekdir(bits->server, bits->handle, &(off_t){ seekpos });
	if(!ok(n == 0, "test seekdir")) diag("n=%d", n);

	/* (this msgbuffer stuff isn't necessary because there's no receive phase,
	 * but it's retained for future testing of other failure modes wrt string
	 * buffer insufficiency and xfer timeouts.)
	 */
	char *dentsbuf = talloc_size(tal, SNEKS_DIRECTORY_DENTSBUF_MAX);
	L4_MsgBuffer_t msgbuf;
	L4_MsgBufferClear(&msgbuf);
	L4_MsgBufferAppendSimpleRcvString(&msgbuf,
		L4_StringItem(SNEKS_DIRECTORY_DENTSBUF_MAX, dentsbuf));
	L4_AcceptStrings(L4_StringItemsAcceptor, &msgbuf);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 3, .X.label = 0xe80a }.raw);
	L4_LoadMR(1, 0xb337);	/* sublabel per api/directory.idl */
	L4_LoadMR(2, bits->handle);
	L4_LoadMR(3, -1);
	L4_MsgTag_t tag = L4_Send_Timeout(bits->server, L4_TimePeriod(20 * 1000));
	L4_Word_t ec = L4_ErrorCode();
	if(!ok(L4_IpcSucceeded(tag), "getdents send")) diag("ec=%lu", ec);

	/* examine entrails. */
	struct sneks_directory_dentry *dat = get_first_dentry(tal, bits, NULL);
	skip_start(!ok(dat != NULL, "test dentry"), 1, "no test data") {
		if(!ok1(streq(dentry_name(ref), dentry_name(dat)))) {
			diag("ref name=`%s', dat name=`%s'", dentry_name(ref),
				dentry_name(dat));
		}
	} skip_end;

	closedir(dir);
	talloc_free(tal);
}
END_TEST

DECLARE_TEST("io:dir", rollback_getdents);

#endif
