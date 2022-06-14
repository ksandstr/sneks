/* TODO: there's a huge amount of copypasta between this and
 * sys/test/harness.c . that should be pared down.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>
#include <assert.h>
#include <ccan/container_of/container_of.h>
#include <ccan/htable/htable.h>
#include <ccan/hash/hash.h>
#include <ccan/str/str.h>
#include <ccan/talloc/talloc.h>
#include <ccan/minmax/minmax.h>
#include <ccan/darray/darray.h>
#include <ccan/autodata/autodata.h>
#include <sneks/test.h>
#include "defs.h"

#define IN_TEST_MAGIC 0xd007beeb /* don't doot beeb because i'm sleep */

struct utest {
	const char *prefix, *group, *name, *id;
	int low, high, pri;
	void (*fn)(int iter);
};

static darray(struct utest *) all_tests = darray_new();

static int htable_dtor(struct htable *ht) { htable_clear(ht); return 0; }

static int cmp_utest_ptr(const void *ap, const void *bp)
{
	const struct utest *a = *(struct utest **)ap, *b = *(struct utest **)bp; /* fuck your yankee const correctness */
	int d;
	if(d = b->pri - a->pri, d != 0) return d; /* descending order of priority */
	if(d = strcmp(a->prefix, b->prefix), d != 0) return d;
	if(d = strcmp(a->group, b->group), d != 0) return d;
	if(d = strcmp(a->name, b->name), d != 0) return d;
	return 0;
}

void done_testing(void) {
	close_no_plan();
	exit(exit_status());
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
static void make_padded_name(char *out, const char *name, size_t take, size_t iter)
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
	struct utest *body;
	darray(struct idcomp *) children; /* dtor */
	struct htable *map; /* talloc & dtor */
	char text[];
};

static int idcomp_dtor(struct idcomp *c) { darray_free(c->children); return 0; }

static size_t hash_idcomp_text(const void *key, void *unused) {
	const struct idcomp *x = key;
	return hash_string(x->text);
}

static bool cmp_idcomp_to_text(const void *cand, void *keyptr) {
	const struct idcomp *x = cand;
	const char *key = keyptr;
	return streq(x->text, key);
}

static struct htable *new_idcomp_map(void *tal) {
	struct htable *map = talloc(tal, struct htable);
	htable_init(map, &hash_idcomp_text, NULL);
	talloc_set_destructor(map, &htable_dtor);
	return map;
}

static struct idcomp *gen_idcomp(size_t length, const char *pfx, struct htable *exset, void *talctx)
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
	*c = (struct idcomp){ .map = new_idcomp_map(c), .children = darray_new() };
	talloc_set_destructor(c, &idcomp_dtor);
	memcpy(c->text, p, plen + 1);
	htable_add(exset, ph, c);
	return c;
}

/* allocates @sts[x]->id within @talctx. other uses of talloc are for
 * convenience only.
 */
static void assign_test_ids(void *talctx, struct utest **sts, size_t n_sts)
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
	for(struct idcomp *px = htable_first(top, &it); px != NULL; px = htable_next(top, &it)) {
		struct utest **s_index = bsearch(&px->body, sts, n_sts, sizeof *sts, &cmp_utest_ptr);
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
			struct utest **g_index = bsearch(&g->body, s_index, p_size, sizeof *sts, &cmp_utest_ptr);
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
				struct idcomp *t = gen_idcomp(name_len, g_index[i]->name, g->map, g->map);
				t->body = g_index[i];
				t->body->id = talloc_asprintf(talctx, "%s%s%s", px->text, g->text, t->text);
			}
		}
		darray_free(groups);
	}
	talloc_free(top);
}

static struct utest *parse_utest(void *tal, const struct utest_spec *spec)
{
	struct utest *t = talloc(tal, struct utest);
	int pathlen = strlen(spec->path);
	char prefix[pathlen + 1], group[pathlen + 1], *colon = strchr(spec->path, ':');
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
	int pri = 0, hi = max(spec->test->iter_low, spec->test->iter_high), lo = min(spec->test->iter_low, spec->test->iter_high);
	*t = (struct utest){
		.prefix = colon != NULL ? talloc_strdup(t, prefix) : NULL, .group = talloc_strdup(t, group), .name = spec->test->name,
		.fn = spec->test->test_fn, .low = lo, .high = hi, .pri = pri,
	};
	assert(t->low <= t->high);
	return t;
}

static struct utest **get_tests(size_t *num_p, bool want_ids)
{
	static void *tal = NULL; /* persistent; used for syntax. */
	if(tal == NULL) tal = talloc_new(NULL);
	/* TODO: parse and sort property lists as well. */
	if(darray_empty(all_tests)) {
		size_t n_specs = 0;
		darray_realloc(all_tests, n_specs);
		struct utest_spec **specs = autodata_get(all_utest_specs, &n_specs);
		for(size_t i=0; i < n_specs; i++) darray_push(all_tests, parse_utest(tal, specs[i]));
		assert(all_tests.size == n_specs);
		qsort(all_tests.item, all_tests.size, sizeof(struct utest *), &cmp_utest_ptr);
	}
	if(want_ids && all_tests.item[0]->id == NULL) {
		assign_test_ids(tal, all_tests.item, all_tests.size);
		assert(all_tests.item[0]->id != NULL);
	}
	*num_p = all_tests.size;
	return all_tests.item;
}

static bool announce(const char **pfx_p, const char **grp_p, const struct utest *t)
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
static void run(const struct utest *t, int iter, bool redo_fixtures)
{
	if(redo_fixtures) {
		/* TODO: stop previous unchecked fixtures here. */
	}
	if(t == NULL) return;
	if(redo_fixtures) {
		/* TODO: start incoming unchecked fixtures here. */
	}

	if(t->low < t->high) {
		printf("*** begin test `%s' iter %d\n", t->name, iter);
	} else {
		printf("*** begin test `%s'\n", t->name);
	}
	int child = fork();
	if(child == 0) {
		/* TODO: setup checked fixtures */
		tap_reset();
		(*t->fn)(iter);
		/* TODO: teardown checked fixtures */
		done_testing();
	}
	int st, pid = waitpid(child, &st, 0);
	if(pid < 0) {
		printf("*** waitpid failed, errno=%d\n", errno);
		abort();
	}
	bool failed = !WIFEXITED(st);
	int rc = WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st);
	if(failed) {
		printf("*** test `%s' failed, rc %d\n", t->name, rc);
	} else {
		printf("*** end test `%s' rc %d\n", t->name, rc);
	}
}

void run_all_tests(void)
{
	size_t n_st = 0;
	struct utest **sts = get_tests(&n_st, false);
	const char *pfx = NULL, *grp = NULL;
	for(int i=0; i < n_st; i++) {
		bool ch = announce(&pfx, &grp, sts[i]);
		for(int iter = sts[i]->low; iter <= sts[i]->high; iter++) run(sts[i], iter, ch);
	}
	announce(&pfx, &grp, NULL);
	run(NULL, -1, true);
}

static size_t hash_utest_by_id(const void *ptr, void *unused) {
	const struct utest *st = ptr;
	return hash_string(st->id);
}

static bool cmp_str_to_utest_id(const void *cand, void *key) {
	const struct utest *st = cand;
	return streq(st->id, key);
}

void run_test_by_spec(char **specs, size_t n_specs)
{
	size_t n_st = 0;
	struct utest **sts = get_tests(&n_st, true);
	struct htable by_id = HTABLE_INITIALIZER(by_id, &hash_utest_by_id, NULL);
	for(size_t i=0; i < n_st; i++) {
		assert(sts[i]->id != NULL);
		htable_add(&by_id, hash_string(sts[i]->id), sts[i]);
	}
	void *talctx = talloc_new(NULL);
	const char *pfx = NULL, *grp = NULL;
	for(size_t i=0; i < n_specs; i++) {
		char *spec = talloc_strdup(talctx, specs[i]);
		/* these ranges could be fancier, i.e. 1-3,7,9-12 and such, but for
		 * now we'll parse just a single iteration number, an asterisk, or no
		 * separator at all.
		 */
		char *colon = strrchr(spec, ':');
		if(colon != NULL) *colon = '\0';
		struct utest *t = htable_get(&by_id, hash_string(spec), &cmp_str_to_utest_id, spec);
		if(t == NULL) { printf("*** unknown test ID `%s'!\n", spec); continue; }
		int lo = t->low, hi = t->high;
		if(colon != NULL && colon[1] >= '0' && colon[1] <= '9') {
			char *end = NULL;
			int sole_iter = strtol(colon + 1, &end, 10);
			if(*end == '\0') lo = hi = sole_iter;
		}
		bool ch = announce(&pfx, &grp, t);
		for(int iter = lo; iter <= hi; iter++) run(t, iter, ch);
	}
	announce(&pfx, &grp, NULL);
	run(NULL, -1, true);
	htable_clear(&by_id);
	talloc_free(talctx);
}

/* NOTE: the multitap protocol is still built around suite/tcase/test. so
 * we'll just cram prefix/group/test in there; it'll be fine.
 */
void describe_all_tests(void)
{
	size_t n_sts = 0;
	struct utest **sts = get_tests(&n_sts, true);
	/* output test IDs and what-not. */
	for(size_t i=0; i < n_sts; i++) {
		if(i == 0 || !streq(sts[i]->prefix, sts[i-1]->prefix)) {
			printf("*** desc suite `%s'\n", sts[i]->prefix);
		}
		if(i == 0 || !streq(sts[i]->group, sts[i-1]->group)) {
			printf("*** desc tcase `%s'\n", sts[i]->group);
		}
		printf("*** desc test `%s' low:%d high:%d id:%s\n", sts[i]->name, sts[i]->low, sts[i]->high, sts[i]->id);
	}
}
