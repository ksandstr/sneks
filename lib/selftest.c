#ifdef BUILD_SELFTEST	/* yeah, the whole module */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <ccan/autodata/autodata.h>
#include <ccan/str/str.h>

#include <l4/types.h>
#include <l4/ipc.h>
#include <l4/kdebug.h>

#include <sneks/process.h>
#include <sneks/systask.h>

#include "muidl.h"


struct wrapctx
{
	L4_ThreadId_t caller, test;
	FILE *oldout, *olderr;
};


static void load_spec(
	int *offset, int *mrpos,
	const char *name, L4_Word_t meta)
{
	if(--*offset < 0) {
		L4_LoadMRs(*mrpos, 5, (L4_Word_t *)name);
		L4_LoadMR(*mrpos + 5, meta);
		*mrpos += 6;
	}
}


static void describe_selftests(L4_ThreadId_t sender, int offset)
{
	size_t n_specs = 0;
	struct utest_spec **specs = autodata_get(all_systask_selftests, &n_specs);

	int mrpos = 1;
	char prefix[21] = "", group[21] = "";
	for(size_t i=0; i < n_specs && mrpos < 58; i++) {
		/* NOTE: this doesn't handle empties well, i.e. "foo:" and ":bar".
		 * don't use those.
		 */
		char *path = strdup(specs[i]->path), *sep = strchr(path, ':');
		if(sep != NULL) *sep = '\0';
		if(strcmp(prefix, path) != 0) {
			strncpy(prefix, path, sizeof prefix - 1); prefix[20] = '\0';
			load_spec(&offset, &mrpos, prefix, 1);
		}
		if(sep != NULL && strcmp(group, sep + 1) != 0) {
			strncpy(group, sep + 1, sizeof group - 1); group[20] = '\0';
			load_spec(&offset, &mrpos, group, 2);
		}
		/* (priority is 0xff at bitpos 26) */
		load_spec(&offset, &mrpos, specs[i]->test->name,
			(specs[i]->test->iter_low & 0xff) << 2
			| (specs[i]->test->iter_high & 0xffff) << 10);
		free(path);
	}

	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = mrpos }.raw);
	L4_MsgTag_t tag = L4_Reply(sender);
	if(L4_IpcFailed(tag)) {
		printf("%s: reply failed, ec=%lu\n", __func__, L4_ErrorCode());
	}
}


static ssize_t wrap_write(void *cookie, const char *buf, size_t size)
{
	/* TODO: tell stdout and stderr apart in both cases */
	struct wrapctx *ctx = cookie;
	if(L4_IsNilThread(ctx->caller)) return 0;
	if(!L4_SameThreads(L4_Myself(), ctx->test)) {
		/* output from other threads in the same process, sharing the same
		 * stdio handles. (which aren't thread safe. oops?)
		 */
		return fwrite(buf, sizeof *buf, size, ctx->oldout);
	}

	/* output through the TAP conduit. */
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_StringItem_t si = L4_StringItem(size, (void *)buf);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1, .X.t = 2 , .X.label = 0 }.raw);
	L4_LoadMR(1, 1);	/* (fd#: 1 for out, 2 for err) */
	L4_LoadMRs(2, 2, si.raw);
	L4_MsgTag_t tag = L4_Call(ctx->caller);
	if(L4_IpcFailed(tag)) {
		L4_KDB_PrintString("selftest wrap_write IPC failed");
		errno = EIO;
		return -1;
	}

	return 0;
}


static int wrap_close(void *cookie)
{
	struct wrapctx *ctx = cookie;
	if(L4_IsNilThread(ctx->caller)) return 0;
	int rc = exit_status();
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1, .X.label = 0xdead }.raw);
	L4_LoadMR(1, rc);
	L4_Send(ctx->caller);
	ctx->caller = L4_nilthread;
	return 0;
}


/* finds first test whose name is a prefix of @name. */
static const struct utest_spec *find_test(const char *name)
{
	size_t n_specs = 0;
	struct utest_spec **specs = autodata_get(all_systask_selftests, &n_specs);
	for(int i=0; i < n_specs; i++) {
		if(strstarts(specs[i]->test->name, name)) return specs[i];
	}
	return NULL;
}


static void run_selftest(L4_ThreadId_t sender, int iter, const char *name)
{
	const struct utest_spec *t = find_test(name);
	if(t == NULL) {
		plan(SKIP_ALL, "test `%s' not found!", name);
		return;
	}

	/* wrap stdio in calls to @sender */
	struct wrapctx ctx = {
		.caller = sender, .test = L4_Myself(),
		.oldout = stdout, .olderr = stderr,
	};
	FILE *wrapout = fopencookie(&ctx, "wb", (cookie_io_functions_t){
		.write = &wrap_write, .close = &wrap_close });
	if(wrapout == NULL) {
		fprintf(stderr, "%s: fopencookie() failed, errno=%d",
			__func__, errno);
		/* pop error up to partner? (it's just malloc failure though.) */
		return;
	}

	/* let's a-go */
	L4_LoadMR(0, 0);
	L4_Reply(sender);

	FILE *oldin = stdin;
	stdin = stdout = stderr = wrapout;

	tap_reset();
	(*t->test->test_fn)(iter);

	stdin = oldin; stdout = ctx.oldout; stderr = ctx.olderr;
	fclose(wrapout);
}


bool selftest_handling(L4_Word_t status)
{
	L4_ThreadId_t sender = muidl_get_sender();
	if(pidof_NP(sender) < SNEKS_MIN_SYSID) return false;

	L4_MsgTag_t tag = muidl_get_tag();
	switch(L4_Label(tag)) {
		default: return false;
		case 0xeffe: {
			L4_Word_t offset; L4_StoreMR(1, &offset);
			describe_selftests(sender, offset);
			return true;
		}
		case 0xeffd: {
			L4_Word_t iter; L4_StoreMR(1, &iter);
			L4_Word_t name[L4_UntypedWords(tag)];
			L4_StoreMRs(2, L4_UntypedWords(tag) - 1, name);
			name[sizeof name / sizeof name[0] - 1] = 0;
			run_selftest(sender, iter, (const char *)name);
			return true;
		}
	}
}
#endif
