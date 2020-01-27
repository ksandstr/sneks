/* systemspace unit testing framework reminiscent of Check, with a big dollop
 * of TAP mixed in.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <threads.h>

#include <ccan/container_of/container_of.h>
#include <ccan/htable/htable.h>
#include <ccan/hash/hash.h>
#include <ccan/str/str.h>
#include <ccan/talloc/talloc.h>
#include <ccan/minmax/minmax.h>
#include <ccan/darray/darray.h>
#include <ccan/autodata/autodata.h>
#include <ccan/array_size/array_size.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>

#include <sneks/test.h>

#include "proc-defs.h"


#define IN_TEST_MAGIC 0xB33B7007	/* quoth the butt canary */


static tss_t in_test_key = ~0;
static darray(struct systest *) all_systests = darray_new();
static const struct systest *current_systest = NULL;


/* TODO: move this into sys/test/util.c or some such. */
static int htable_dtor(struct htable *ht) {
	htable_clear(ht);
	return 0;
}


static int cmp_systest_ptr(const void *ap, const void *bp)
{
	/* const correctness is a bourgeois concept */
	const struct systest *a = *(struct systest **)ap,
		*b = *(struct systest **)bp;

	int diff = b->pri - a->pri;	/* priority desc */
	if(diff != 0) return diff;
	diff = strcmp(a->prefix, b->prefix);	/* prefix asc */
	if(diff != 0) return diff;
	diff = strcmp(a->group, b->group);	/* group asc */
	if(diff != 0) return diff;
	diff = strcmp(a->name, b->name);		/* name asc */
	if(diff != 0) return diff;

	return 0;
}


bool in_test(void) {
	return tss_get(in_test_key) == (void *)IN_TEST_MAGIC;
}


static int cmp_strptr(const void *ap, const void *bp) {
	const char *a = *(const char **)ap, *b = *(const char **)bp;
	return strcmp(a, b);
}


/* compute longest common name prefix within @strs. */
static size_t prefix_length(const char *strs[], size_t n_strs)
{
	char *names[n_strs];
	memcpy(names, strs, sizeof strs[0] * n_strs);
	qsort(names, n_strs, sizeof(char *), &cmp_strptr);

	size_t max_prefix = 0;
	for(size_t i=1; i < n_strs; i++) {
		const char *prev = names[i - 1], *cur = names[i];
		size_t p = 0;
		while(prev[p] == cur[p] && prev[p] != '\0') p++;
		max_prefix = max_t(size_t, p, max_prefix);
	}

	return max_prefix;
}


/* grab up to @take - [# of digits in @iter if > 0] alpha characters from
 * @name, capitalize the first letter, and add @iter converted to a base-10
 * number (of at most 2 digits). the @iter goes on top of the last two if
 * the result would otherwise be longer than 4 characters.
 */
static void make_padded_name(
	char *out, const char *name, size_t take, size_t iter)
{
	char tmp[take + 1];
	int t = 0;
	for(int i = 0; name[i] != '\0' && t < take; i++) {
		if(isalpha(name[i])) tmp[t++] = name[i];
	}
	tmp[t] = '\0';
	if(iter == 0) {
		memcpy(out, tmp, t + 1);
	} else if(iter <= 10) {
		if(take > 3) tmp[--t] = '\0';
		snprintf(out, take + 2, "%s%d", tmp, iter - 1);
	} else if(iter <= 100) {
		if(take > 3) tmp[--t] = '\0';
		if(take > 2) tmp[--t] = '\0';
		snprintf(out, take + 3, "%s%02d", tmp, iter - 1);
	} else {
		assert(iter <= 100);
	}
	out[0] = toupper(out[0]);
}


/* identifier component structure. talloc'd for attached stuff. */
struct idcomp {
	struct systest *body;
	darray(struct idcomp *) children;	/* dtor */
	struct htable *map;			/* talloc & dtor */
	char text[];
};


static int idcomp_dtor(struct idcomp *c) {
	darray_free(c->children);
	return 0;
}


static size_t hash_idcomp_text(const void *key, void *unused) {
	const struct idcomp *x = key;
	return hash_string(x->text);
}


static bool cmp_idcomp_to_text(const void *cand, void *keyptr) {
	const struct idcomp *x = cand;
	const char *key = keyptr;
	return streq(x->text, key);
}


static struct htable *new_idcomp_map(void *tal)
{
	struct htable *map = talloc(tal, struct htable);
	htable_init(map, &hash_idcomp_text, NULL);
	talloc_set_destructor(map, &htable_dtor);
	return map;
}


static struct idcomp *gen_idcomp(
	size_t length, const char *pfx,
	struct htable *exset, void *talctx)
{
	bool ok = false;
	char p[length + 32];
	size_t plen, ph;
	for(int iter=0; iter < 100; iter++) {
		make_padded_name(p, pfx, min_t(size_t, length, 4) + 1, iter);
		plen = strlen(p);
		ph = hash_string(p);
		if(htable_get(exset, ph, &cmp_idcomp_to_text, p) == NULL) {
			ok = true;
			break;
		}
	}
	if(!ok) return NULL;

	struct idcomp *c = talloc_size(talctx, sizeof(struct idcomp) + plen + 1);
	*c = (struct idcomp){
		.map = new_idcomp_map(c),
		.children = darray_new(),
	};
	talloc_set_destructor(c, &idcomp_dtor);
	memcpy(c->text, p, plen + 1);
	htable_add(exset, ph, c);

	return c;
}


/* allocates @sts[x]->id within @talctx. other uses of talloc are for
 * convenience only.
 */
static void assign_test_ids(void *talctx, struct systest **sts, size_t n_sts)
{
	/* generate toplevel shorthands. */
	struct htable *top = new_idcomp_map(NULL);
	const char *strs[n_sts];
	size_t n_strs = 0;
	for(size_t i=0; i < n_sts; i++) {
		const char *pfx = sts[i]->prefix;
		if(i == 0 || !streq(pfx, sts[i - 1]->prefix)) strs[n_strs++] = pfx;
		assert(n_strs <= n_sts);
	}
	size_t top_len = prefix_length(strs, n_strs);
	struct idcomp *cur_suite = NULL;
	for(size_t i=0; i < n_sts; i++) {
		const char *pfx = sts[i]->prefix;
		if(cur_suite != NULL && streq(pfx, cur_suite->body->prefix)) continue;

		cur_suite = gen_idcomp(top_len, pfx, top, top);
		cur_suite->body = sts[i];
	}

	/* generate group shorthands per toplevel prefix. */
	struct htable_iter it;
	for(struct idcomp *px = htable_first(top, &it);
		px != NULL;
		px = htable_next(top, &it))
	{
		struct systest **s_index = bsearch(&px->body, sts,
			n_sts, sizeof *sts, &cmp_systest_ptr);
		assert(s_index != NULL);
		assert(*s_index == px->body);

		n_strs = 0;
		int p_size = 0;
		for(int i=0; &s_index[i] < &sts[n_sts]; i++, p_size++) {
			if(!streq(s_index[i]->prefix, px->body->prefix)) break;
			if(i == 0 || !streq(s_index[i-1]->group, s_index[i]->group)) {
				assert(n_strs < n_sts);
				strs[n_strs++] = s_index[i]->group;
			}
		}
		int grp_len = prefix_length(strs, n_strs);

		darray(struct idcomp *) groups = darray_new();
		for(int i=0; i < p_size; i++) {
			const char *grp = s_index[i]->group;
			if(i > 0 && streq(s_index[i - 1]->group, grp)) continue;
			struct idcomp *g = gen_idcomp(grp_len, grp, px->map, px->map);
			g->body = s_index[i];
			darray_push(groups, g);
		}

		/* generate test shorthands per prefixes of toplevel and group. */
		for(int grp_ix=0; grp_ix < groups.size; grp_ix++) {
			struct idcomp *g = groups.item[grp_ix];
			struct systest **g_index = bsearch(&g->body, s_index,
				p_size, sizeof *sts, &cmp_systest_ptr);
			assert(g_index != NULL);
			assert(*g_index == g->body);

			n_strs = 0;
			int g_size = 0;
			for(int i=0; &g_index[i] < &s_index[p_size]; i++, g_size++) {
				if(!streq(g_index[i]->group, g->body->group)) break;
				if(!streq(g_index[i]->prefix, g->body->prefix)) break;
				assert(n_strs < n_sts);
				strs[n_strs++] = g_index[i]->name;
			}
			int name_len = prefix_length(strs, n_strs);

			for(int i=0; i < g_size; i++) {
				struct idcomp *t = gen_idcomp(name_len, g_index[i]->name,
					g->map, g->map);
				t->body = g_index[i];
				t->body->id = talloc_asprintf(talctx, "%s%s%s",
					px->text, g->text, t->text);
			}
		}

		darray_free(groups);
	}

	talloc_free(top);
}


static struct systest *parse_systest(
	void *tal, const struct systest_spec *spec)
{
	struct systest *t = talloc(tal, struct systest);

	int pathlen = strlen(spec->path);
	char prefix[pathlen + 1], group[pathlen + 1],
		*colon = strchr(spec->path, ':');
	if(colon != NULL) {
		int plen = colon - spec->path;
		assert(plen > 0 && plen < pathlen);
		memcpy(prefix, spec->path, plen);
		prefix[plen] = '\0';
		strscpy(group, colon + 1, pathlen + 1);
	} else {
		prefix[0] = '\0';
		memcpy(group, spec->path, pathlen + 1);
	}
	/* FIXME: get this from a property */
	int pri = 0,
		hi = max(spec->test->iter_low, spec->test->iter_high),
		lo = min(spec->test->iter_low, spec->test->iter_high);

	*t = (struct systest){
		.prefix = colon != NULL ? talloc_strdup(t, prefix) : NULL,
		.group = talloc_strdup(t, group),
		.name = spec->test->name,
		.low = lo, .high = hi, .pri = pri,
		.fn = spec->test->test_fn,
	};
	assert(t->low <= t->high);
	return t;
}


#ifdef BUILD_SELFTEST

struct selftest_info {
	L4_ThreadId_t tid;
};


static void run_systask_selftest(int iter)
{
	struct selftest_info *inf = (void *)(current_systest + 1);
	diag("[wrapper running `%s' on %lu:%lu]", current_systest->name,
		L4_ThreadNo(inf->tid), L4_Version(inf->tid));
	int name_words = (strlen(current_systest->name)
		+ sizeof(L4_Word_t) - 1) / sizeof(L4_Word_t);
	L4_LoadMR(0, (L4_MsgTag_t){
		.X.u = 1 + name_words,
		.X.label = 0xeffd }.raw);
	L4_LoadMR(1, iter);
	L4_LoadMRs(2, name_words, (const L4_Word_t *)current_systest->name);
	L4_MsgTag_t tag = L4_Call(inf->tid);
	if(L4_IpcFailed(tag) || L4_Label(tag) == 1) {
		L4_Word_t code; L4_StoreMR(1, &code);
		plan_skip_all("IPC failed, ec=%lu, code=%#lx",
			L4_ErrorCode(), code);
		return;
	}

	/* passthru I/O from test process. */
	char iobuf[513];
	L4_MsgBuffer_t msgbuf; L4_MsgBufferClear(&msgbuf);
	L4_MsgBufferAppendSimpleRcvString(&msgbuf,
		L4_StringItem(sizeof iobuf, iobuf));
	bool running = true;
	while(running) {
		L4_AcceptStrings(L4_StringItemsAcceptor, &msgbuf);
		tag = L4_Receive(inf->tid);
		for(;;) {
			if(L4_IpcFailed(tag)) {
				diag("[wrapper IPC fail, ec=%lu]", L4_ErrorCode());
				break;
			} else if(L4_Label(tag) == 0xdead) {
				L4_Word_t rc; L4_StoreMR(1, &rc);
				diag("[wrapper exit, rc=%d]", (int)rc);
				running = false;
				break;
			} else if(L4_Label(tag) != 0) {
				diag("[wrapper got unknown label=%#lx]", L4_Label(tag));
				break;
			}

			L4_Word_t fd; L4_StoreMR(1, &fd);
			/* decodes the first substring of the first string item, which
			 * must be simple.
			 */
			L4_StringItem_t si; L4_StoreMRs(2, 2, si.raw);
			int len = si.X.string_length;
			assert(len <= sizeof iobuf);
			iobuf[min_t(int, len, sizeof iobuf - 1)] = '\0';
			size_t n = fwrite(iobuf, 1, min_t(int, len, sizeof iobuf - 1),
				fd == 1 ? stdout : stderr);
			L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1 }.raw);
			L4_LoadMR(1, n);
			L4_ThreadId_t dummy;	/* my assclap keeps alerting the bees! */
			L4_AcceptStrings(L4_StringItemsAcceptor, &msgbuf);
			tag = L4_Ipc(inf->tid, inf->tid,
				L4_Timeouts(L4_ZeroTime, L4_Never), &dummy);
		}
	}
	L4_Accept(L4_UntypedWordsAcceptor);
}


union stdesc {
	L4_Word_t raw[6];
	struct {
		char name[20];
		L4_Word_t meta;
	} body;
};


static struct systest *parse_stdesc(
	void *tal, const char *prefix, const char *group,
	const union stdesc *d, L4_ThreadId_t tid)
{
	struct systest *st = talloc_size(tal,
		sizeof *st + sizeof(struct selftest_info));
	*st = (struct systest){
		.prefix = talloc_strdup(st, prefix),
		.group = talloc_strdup(st, group),
		.name = talloc_strndup(st, d->body.name, 20),
		.low = (d->body.meta >> 2) & 0xff,
		.high = (d->body.meta >> 10) & 0xffff,
		.pri = (d->body.meta >> 26) & 0xff,
		.fn = &run_systask_selftest,
	};
	struct selftest_info *inf = (void *)(st + 1);
	*inf = (struct selftest_info){ .tid = tid };
	assert(st->low <= st->high);
	return st;
}


static bool query_selftests(void *tal, L4_ThreadId_t tid)
{
	char prefix[21], group[21];
	int offset = 0, n_descs;
	do {
		L4_Accept(L4_UntypedWordsAcceptor);
		L4_MsgTag_t tag = { .X.u = 1, .X.label = 0xeffe };
		L4_LoadMR(0, tag.raw);
		L4_LoadMR(1, offset);
		/* TODO: replace receive timeout with ZeroTime once mung stops
		 * breaking it immediately
		 */
		tag = L4_Call_Timeouts(tid, L4_TimePeriod(8000), L4_TimePeriod(5000));
		if(L4_IpcFailed(tag) || L4_UntypedWords(tag) == 1) {
			/* (error code in MR1. ignore it.) */
			break;
		}

		union stdesc desc[10];
		n_descs = min_t(int, L4_UntypedWords(tag) / 6, ARRAY_SIZE(desc));
		L4_StoreMRs(1, n_descs * 6, desc[0].raw);
		for(int i=0; i < n_descs; i++) {
			switch(desc[i].body.meta & 3) {
				case 0:
					darray_push(all_systests,
						parse_stdesc(tal, prefix, group, &desc[i], tid));
					break;
				case 1:
					memcpy(prefix, desc[i].body.name, 20); prefix[20] = '\0';
					continue;
				case 2:
					memcpy(group, desc[i].body.name, 20); group[20] = '\0';
					continue;
				case 3:
					/* unused */
					break;
			}
		}
		offset += n_descs;
	} while(n_descs == 10);

	return offset > 0;
}
#endif


/* get selftest metadata from systasks where applicable. */
static void get_systask_selftests(void *tal)
{
#ifdef BUILD_SELFTEST
	int last_pid = 0, skip, prev_pid = 0;
	for(;;) {
		extern L4_ThreadId_t __uapi_tid;	/* from sys/crt (?) */
		L4_Word_t tidbuf[63];
		unsigned n_tids = ARRAY_SIZE(tidbuf);
		int n = __proc_get_systask_threads(__uapi_tid, &skip,
			tidbuf, &n_tids, last_pid, skip);
		if(n != 0) {
			printf("%s: n=%d\n", __func__, n);
			abort();
		}
		if(n_tids == 0) break;
		L4_ThreadId_t tid = L4_nilthread;
		for(int i=0; i < n_tids; i++) {
			tid.raw = tidbuf[i];
			if(pidof_NP(tid) < prev_pid) continue;
			if(L4_SameThreads(L4_Myself(), tid)) continue;	/* oof! */
			if(query_selftests(tal, tid)) prev_pid = pidof_NP(tid);
		}
		last_pid = pidof_NP(tid) + (skip > 0 ? 0 : 1);
		//printf("%s: skip'=%d, last_pid'=%d\n", __func__, skip, last_pid);
	}
#endif
}


static struct systest **get_systests(size_t *num_p, bool want_ids)
{
	static void *tal = NULL; /* persistent; used for syntax. */
	if(tal == NULL) tal = talloc_new(NULL);

	/* TODO: parse and sort the property lists as well. */

	if(darray_empty(all_systests)) {
		size_t n_specs = 0;
		darray_realloc(all_systests, n_specs);
		struct systest_spec **specs = autodata_get(all_systests, &n_specs);
		for(size_t i=0; i < n_specs; i++) {
			darray_push(all_systests, parse_systest(tal, specs[i]));
		}
		assert(all_systests.size == n_specs);
		get_systask_selftests(tal);
		qsort(all_systests.item, all_systests.size,
			sizeof(struct systest *), &cmp_systest_ptr);
	}

	if(want_ids && all_systests.item[0]->id == NULL) {
		assign_test_ids(tal, all_systests.item, all_systests.size);
		assert(all_systests.item[0]->id != NULL);
	}

	*num_p = all_systests.size;
	return all_systests.item;
}


static bool announce(
	const char **pfx_p, const char **grp_p,
	const struct systest *t)
{
	const char *pfx = *pfx_p, *grp = *grp_p;
	bool changed = false;
	if(t == NULL) {
		if(grp != NULL) {
			printf("*** end tcase `%s'\n", grp);
			grp = NULL; changed = true;
		}
		if(pfx != NULL) {
			printf("*** end suite `%s'\n", pfx);
			pfx = NULL; changed = true;
		}
	} else {
		if(pfx == NULL || !streq(t->prefix, pfx)) {
			changed = true;
			if(grp != NULL) {
				printf("*** end tcase `%s'\n", grp);
				grp = NULL;
			}
			if(pfx != NULL) printf("*** end suite `%s'\n", pfx);
			pfx = t->prefix;
			printf("*** begin suite `%s'\n", pfx);
		}
		if(grp == NULL || !streq(t->group, grp)) {
			changed = true;
			if(grp != NULL) printf("*** end tcase `%s'\n", grp);
			grp = t->group;
			printf("*** begin tcase `%s'\n", grp);
		}
	}
	*grp_p = grp;
	*pfx_p = pfx;
	return changed;
}


/* TODO: @redo_fixtures is for restarting persistent per-group fixtures when
 * either group or prefix changed between tests. it's currently unused since
 * there's no fixture support here.
 *
 * also, per a previous comment, fixture-starting code should be run in test
 * context (i.e. threads) so that fixture setup and teardown code can call
 * fail_if() etc. like any other test.
 */
static void run(const struct systest *t, int iter, bool redo_fixtures)
{
	if(redo_fixtures) {
		/* TODO: stop previous persistent fixtures here. */
	}
	if(t == NULL) return;
	if(redo_fixtures) {
		/* TODO: start incoming persistent fixtures here. */
	}

	if(t->low < t->high) {
		printf("*** begin test `%s' iter %d\n", t->name, iter);
	} else {
		printf("*** begin test `%s'\n", t->name);
	}
	/* TODO: start per-test fixtures */

	tap_reset();
	/* TODO: run tests in a thread for each to permit abnormal exits, segfault
	 * catching, enforce maximum test walltime, and so forth.
	 */
	bool failed = false;
	assert(current_systest == NULL);
	current_systest = t;
	(*t->fn)(iter);
	current_systest = NULL;
	int rc = exit_status();
	/* TODO: stop per-test fixtures */
	if(failed) {
		printf("*** test `%s' failed, rc %d\n", t->name, rc);
	} else {
		printf("*** end test `%s' rc %d\n", t->name, rc);
	}
}


void run_all_systests(void)
{
	size_t n_st = 0;
	struct systest **sts = get_systests(&n_st, false);
	const char *pfx = NULL, *grp = NULL;
	for(int i=0; i < n_st; i++) {
		bool ch = announce(&pfx, &grp, sts[i]);
		for(int iter = sts[i]->low; iter <= sts[i]->high; iter++) {
			run(sts[i], iter, ch);
		}
	}
	announce(&pfx, &grp, NULL);
	run(NULL, -1, true);
}


static size_t hash_systest_by_id(const void *ptr, void *unused) {
	const struct systest *st = ptr;
	return hash_string(st->id);
}


static bool cmp_str_to_systest_id(const void *cand, void *key) {
	const struct systest *st = cand;
	return streq(st->id, key);
}


void run_systest_by_spec(char **specs, size_t n_specs)
{
	size_t n_st = 0;
	struct systest **sts = get_systests(&n_st, true);

	struct htable by_id = HTABLE_INITIALIZER(by_id,
		&hash_systest_by_id, NULL);
	for(size_t i=0; i < n_st; i++) {
		assert(sts[i]->id != NULL);
		htable_add(&by_id, hash_string(sts[i]->id), sts[i]);
	}

	const char *pfx = NULL, *grp = NULL;
	for(size_t i=0; i < n_specs; i++) {
		char *spec = strdup(specs[i]);

		/* FIXME: add fancy ranges and so forth. for now we'll ignore them. */
		char *colon = strrchr(spec, ':');
		if(colon != NULL) *colon = '\0';

		struct systest *t = htable_get(&by_id, hash_string(spec),
			&cmp_str_to_systest_id, spec);
		if(t == NULL) {
			printf("*** unknown test ID `%s'!\n", spec);
		} else {
			bool ch = announce(&pfx, &grp, t);
			for(int iter = t->low; iter <= t->high; iter++) run(t, iter, ch);
		}
		free(spec);
	}
	announce(&pfx, &grp, NULL);
	run(NULL, -1, true);

	htable_clear(&by_id);
}


/* NOTE: the multitap protocol is still built around suite/tcase/test. so
 * we'll just cram prefix/group/test in there; it'll be fine.
 */
void describe_all_systests(void)
{
	size_t n_sts = 0;
	struct systest **sts = get_systests(&n_sts, true);

	/* output test IDs and what-not. */
	for(size_t i=0; i < n_sts; i++) {
		if(i == 0 || !streq(sts[i]->prefix, sts[i-1]->prefix)) {
			printf("*** desc suite `%s'\n", sts[i]->prefix);
		}
		if(i == 0 || !streq(sts[i]->group, sts[i-1]->group)) {
			printf("*** desc tcase `%s'\n", sts[i]->group);
		}
		printf("*** desc test `%s' low:%d high:%d id:%s\n",
			sts[i]->name, sts[i]->low, sts[i]->high, sts[i]->id);
	}
}
