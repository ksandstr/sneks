#define SYSMSG_IMPL_SOURCE
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <muidl.h>
#include <ccan/array_size/array_size.h>
#include <ccan/compiler/compiler.h>
#include <ccan/htable/htable.h>
#include <ccan/list/list.h>
#include <ccan/minmax/minmax.h>
#include <ccan/siphash/siphash.h>
#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>
#include <sneks/hash.h>
#include <sneks/bitops.h>
#include <sneks/process.h>
#include <sneks/systask.h>
#include <sneks/sys/msg-defs.h>

#define FILTER_SIZE (64 / sizeof(L4_Word_t))	/* one cacheline's worth */
#define SLOTS_PER_LIMB (sizeof(L4_Word_t) * 8 / 4)

/* TODO: into a lib/ utility header! */
#define for_each_bit(b, mask) /* long version */ \
	for(long _acc = (mask), b = ffsl(_acc) - 1; _acc != 0; _acc &= ~(1l << b), b = ffsl(_acc) - 1)

struct msgclient {
	L4_ThreadId_t recv_tid;
	int subs_mask, filter_mask, n_filters;
	L4_Word_t *filter;	/* 4-bit counting 3-hash bloom filter */
	struct list_head subs;
};

struct sub {
	struct list_node link;
	struct msgclient *client;
	short bit;
};

static size_t rehash_msgclient(const void *, void *);
static size_t rehash_sub(const void *, void *);
static L4_MsgTag_t recursive_sysmsg_call(L4_MsgTag_t tag, L4_ThreadId_t sender);
static bool filter_pass(L4_Word_t *filter, int maskp, uint32_t hash);

static struct htable client_ht = HTABLE_INITIALIZER(client_ht, &rehash_msgclient, NULL),
	subs_ht = HTABLE_INITIALIZER(subs_ht, &rehash_sub, NULL);
static L4_ThreadId_t main_tid;
static L4_Word_t filter_salt[32];

static size_t rehash_msgclient(const void *ptr, void *priv UNUSED) {
	return int_hash(pidof_NP(((const struct msgclient *)ptr)->recv_tid));
}

static size_t rehash_sub(const void *item, void *priv UNUSED) {
	const struct sub *s = item;
	return int_hash(s->bit);
}

static bool cmp_pid_to_msgclient(const void *cand, void *key) {
	const struct msgclient *c = cand;
	return pidof_NP(c->recv_tid) == *(int *)key;
}

static int order_msgclientptr_by_pid(const void *a, const void *b) {
	const struct msgclient *l = *(const struct msgclient **)a, *r = *(const struct msgclient **)b;
	return pidof_NP(l->recv_tid) - pidof_NP(r->recv_tid);
}

static struct msgclient *get_client(L4_ThreadId_t tid) {
	int pid = pidof_NP(tid);
	return htable_get(&client_ht, int_hash(pid), &cmp_pid_to_msgclient, &pid);
}

static void rmsub(struct msgclient *c, int bit)
{
	size_t hash = int_hash(bit);
	struct htable_iter it;
	for(struct sub *s = htable_firstval(&subs_ht, &it, hash); s != NULL; s = htable_nextval(&subs_ht, &it, hash)) {
		if(s->bit != bit || s->client != c) continue;
		list_del_from(&c->subs, &s->link);
		htable_del(&subs_ht, hash, s);
		free(s);
	}
}

static void addsub(struct msgclient *c, int bit)
{
	struct sub *s = malloc(sizeof *s);
	if(s == NULL) abort();
	*s = (struct sub){ .client = c, .bit = bit };
	if(!htable_add(&subs_ht, rehash_sub(s, NULL), s)) abort();
	list_add_tail(&c->subs, &s->link);
}

static int setmask(L4_ThreadId_t recv_tid, int or, int and)
{
	struct msgclient *c = get_client(recv_tid);
	if(c == NULL) {
		if(and == 0 && or == 0) return 0;
		if(c = malloc(sizeof *c), c == NULL) abort();
		*c = (struct msgclient){ .recv_tid = recv_tid };
		if(!htable_add(&client_ht, rehash_msgclient(c, NULL), c)) abort();
		list_head_init(&c->subs);
	}
	int old_mask = c->subs_mask;
	c->subs_mask = (c->subs_mask & and) | or;
	int removed = old_mask & ~c->subs_mask, added = ~old_mask & c->subs_mask;
	c->filter_mask &= ~removed;
	for_each_bit(b, removed) { rmsub(c, b); }
	for_each_bit(b, added) { addsub(c, b); }
	return old_mask;
}

static int impl_setmask(int or, int and) {
	return setmask(muidl_get_sender(), or, and);
}

static int send_to_client(struct msgclient *c, L4_Word_t bits, const L4_Word_t *body, size_t body_len)
{
	assert(!L4_SameThreads(muidl_get_sender(), c->recv_tid));
	L4_MsgTag_t tag = (L4_MsgTag_t){ .X.label = 0xe807, .X.u = body_len + 1 };
	L4_Set_VirtualSender(main_tid);
	L4_Set_Propagation(&tag);
	L4_LoadMR(0, tag.raw);
	L4_LoadMR(1, bits);
	L4_LoadMRs(2, body_len, body);
	L4_Accept(L4_UntypedWordsAcceptor);
	tag = L4_Call(c->recv_tid);
	while(L4_IpcSucceeded(tag) && L4_Label(tag) == 0xe007) {
		tag = recursive_sysmsg_call(tag, c->recv_tid);
	}
	/* this may cause funny behaviour in that an IPC failure in a Sysmsg call
	 * from within a handler could cause the outer handler call to fail also,
	 * which may in turn lead to handler replies appearing in the muidl
	 * dispatch loop.
	 */
	if(L4_IpcFailed(tag)) return -(int)L4_ErrorCode();
	else {
		assert(L4_UntypedWords(tag) == 1);
		L4_Word_t st; L4_StoreMR(1, &st);
		return st;
	}
}

static bool impl_broadcast(int maskp, int maskn, const L4_Word_t *body, unsigned body_len)
{
	if(maskp == 0 || maskn == ~0) return true;
	struct msgclient *recbuf[64], **recs = recbuf;
	int sz = 0, alloc = ARRAY_SIZE(recbuf), count = 0, immediate = true;
	for_each_bit(bit, maskp) {
		size_t hash = int_hash(bit);
		struct htable_iter it;
		for(struct sub *s = htable_firstval(&subs_ht, &it, hash); s != NULL; s = htable_nextval(&subs_ht, &it, hash)) {
			if(s->bit != bit || (s->client->subs_mask & maskn)) continue;
			if(sz == alloc) {
				if(recs == recbuf && (recs = memdup(recbuf, sz * sizeof *recs), recs == NULL)) abort();
				alloc = max(16, alloc * 2);
				recs = realloc(recs, alloc * sizeof *recs);
				if(recs == NULL) abort();
			}
			recs[sz++] = s->client;
		}
	}
	qsort(recs, sz, sizeof *recs, &order_msgclientptr_by_pid);
	if(sz > 0) count++;
	for(int i = 1; i < sz; i++) { /* uniq */
		if(order_msgclientptr_by_pid(&recs[i - 1], &recs[i]) != 0) recs[count++] = recs[i];
	}
	uint32_t hash = int_hash(body[0]);
	for(int i = 0; i < count; i++) {
		assert(i < 1 || recs[i - 1] != recs[i]);
		int f = recs[i]->filter_mask; /* has to pass at least one bit in maskp. */
		if((f & maskp) == maskp && !filter_pass(recs[i]->filter, maskp & f, hash)) continue;
		int n = send_to_client(recs[i], maskp, body, body_len);
		if(n == 1) immediate = false;
		if(n < 0) log_err("couldn't send to pid=%d, n=%d", pidof_NP(recs[i]->recv_tid), n);
	}
	if(recs != recbuf) free(recs);
	return immediate;
}

/* four-bit unsigned saturating increment. returns true when the field became
 * saturated, false if it was already or is afterward at non-saturation. each
 * field is 4 bits wide for maximum reversibility.
 */
static bool filter_inc(L4_Word_t *filter, int slot) {
	int limb = slot / SLOTS_PER_LIMB, offs = (slot % SLOTS_PER_LIMB) * 4;
	bool sat = ((filter[limb] >> offs) & 0xf) == 0xe;
	if(((filter[limb] >> offs) & 0xf) < 0xf) filter[limb] += 1lu << offs;
	return sat;
}

/* four-bit unsigned saturation-preserving decrement. also preserves zeroes,
 * even though the sole call site's use of filter_query() ensures they never
 * appear.
 */
static void filter_dec(L4_Word_t *filter, int slot) {
	int limb = slot / SLOTS_PER_LIMB, offs = (slot % SLOTS_PER_LIMB) * 4;
	if((((filter[limb] >> offs) + 1) & 0xf) > 1) filter[limb] -= 1lu << offs;
}

static void get_slots(int slots[static 3], uint32_t hash)
{
	const int n_slots = FILTER_SIZE * sizeof(L4_Word_t) * 8 / 4;
	for(int i=0; i < 3; i++) {
		slots[i] = hash & (n_slots - 1);
		hash /= n_slots;
	}
	/* TODO: make slots[0..2] distinct */
}

static bool filter_query(L4_Word_t *filter, const int slots[static 3])
{
	bool ret = true;
	for(int i=0; i < 3; i++) {
		int limb = slots[i] / SLOTS_PER_LIMB, offs = (slots[i] % SLOTS_PER_LIMB) * 4;
		ret &= !!(filter[limb] & (0xf << offs));
	}
	return ret;
}

static bool filter_pass(L4_Word_t *filter, int maskp, uint32_t hash)
{
	if(filter == NULL) return true;
	for_each_bit(bit, maskp) {
		int slots[3]; get_slots(slots, hash ^ filter_salt[bit]);
		if(filter_query(filter, slots)) return true;
	}
	return false;
}

#ifdef BUILD_SELFTEST
#include <sneks/test.h>

/* test that the filter saturates the addressed slot, doesn't affect any
 * slot besides, and returns the saturation result correctly.
 */
START_TEST(filter_inc)
{
	plan_tests(7);

	L4_Word_t filter[FILTER_SIZE];
	memset(filter, 0, sizeof filter);
	ok(filter[1] == 0, "starts out zero");

	bool sat = filter_inc(filter, 12);
	if(!ok(filter[1] == 0x00010000, "incremented to one")) {
		diag("filter[1]=%#lx", filter[1]);
	}
	ok1(!sat);

	/* bump it 13 times more to put it at 0xe. */
	for(int i=0; i < 13; i++) sat = filter_inc(filter, 12);
	if(!ok(filter[1] == 0x000e0000, "incremented to fourteen")) {
		diag("filter[1]=%#lx", filter[1]);
	}
	ok1(!sat);

	/* then once more to 0xf, which should saturate. */
	sat = filter_inc(filter, 12);
	ok(sat && filter[1] == 0x000f0000, "indicated becoming saturated");

	/* subsequently the value should remain. */
	sat = filter_inc(filter, 12);
	ok(!sat && filter[1] == 0x000f0000, "remained saturated");
}
END_TEST

SYSTASK_SELFTEST("sysmsg:bloom", filter_inc);

START_TEST(filter_dec)
{
	plan_tests(4);

	L4_Word_t filter[FILTER_SIZE];
	memset(filter, 0, sizeof filter);
	ok(filter[1] == 0, "starts out zero");

	filter_dec(filter, 12);
	ok(filter[1] == 0, "doesn't alter zero");
	for(int i=0; i < 15; i++) filter_inc(filter, 12);
	ok(filter[1] == 0x000f0000, "test value saturated");
	filter_dec(filter, 12);
	if(!ok(filter[1] == 0x000f0000, "saturated value remains")) {
		diag("filter[1]=%#lx", filter[1]);
	}
}
END_TEST

SYSTASK_SELFTEST("sysmsg:bloom", filter_dec);

#endif

static void add_filter(L4_ThreadId_t sender, int mask, const L4_Word_t *labels, unsigned n_labels)
{
	struct msgclient *c = get_client(sender);
	if(c == NULL) {
		/* clients must start with a call to setmask, or not be recognized. */
		log_err("no client for pid=%d (%lu:%lu)?", pidof_NP(sender), L4_ThreadNo(sender), L4_Version(sender));
		return;
	}
	if(c->filter == NULL) {
		assert(c->n_filters == 0);
		if(c->filter = calloc(FILTER_SIZE, sizeof *c->filter), c->filter == NULL) {
			log_err("can't allocate client filter!");
			/* FIXME: set some kind of an all-saturated filter, and don't free
			 * it in msgclient_dtor()?
			 */
			return;
		}
	}
	for(int i=0; i < n_labels; i++) {
		uint32_t hash = int_hash(labels[i]);
		for_each_bit(bit, mask) {
			int slots[3]; get_slots(slots, hash ^ filter_salt[bit]);
			for(int j=0; j < 3; j++) filter_inc(c->filter, slots[j]);
		}
		c->filter_mask |= mask;
	}
	c->n_filters += n_labels * __builtin_popcount(mask);
}

static void impl_add_filter(int mask, const L4_Word_t *labels, unsigned n_labels) {
	return add_filter(muidl_get_sender(), mask, labels, n_labels);
}

static void rm_filter(L4_ThreadId_t sender, int mask, const L4_Word_t *labels, unsigned n_labels)
{
	struct msgclient *c = get_client(sender);
	if(c == NULL || c->filter == NULL) return;
	int removed = 0;
	for(int i=0; i < n_labels; i++) {
		uint32_t hash = int_hash(labels[i]);
		for_each_bit(bit, mask) {
			int slots[3]; get_slots(slots, hash ^ filter_salt[bit]);
			if(!filter_query(c->filter, slots)) continue;
			for(int j=0; j < 3; j++) filter_dec(c->filter, slots[j]);
			removed++;
		}
	}
	assert(removed <= c->n_filters);
	c->n_filters -= removed;
	if(c->n_filters == 0) {
		free(c->filter);
		c->filter = NULL;
	}
}

static void impl_rm_filter(int mask, const L4_Word_t *labels, unsigned n_labels) {
	return rm_filter(muidl_get_sender(), mask, labels, n_labels);
}

static L4_MsgTag_t recursive_sysmsg_call(L4_MsgTag_t tag, L4_ThreadId_t sender)
{
	L4_ThreadId_t dummy;
	int err;

	assert(L4_Label(tag) == 0xe007);
	L4_Word_t oplabel; L4_StoreMR(1, &oplabel);
	switch(oplabel) {
		case 1: /* setmask */ {
			L4_Word_t or, and;
			L4_StoreMR(1, &or);
			L4_StoreMR(2, &and);
			int oldmask = setmask(sender, or, and);
			L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1 }.raw);
			L4_LoadMR(1, oldmask);
			break;
		}
		case 2: /* broadcast */
			/* (this cannot be implemented recursively without pushing an
			 * "EAGAIN" condition down to the caller; however, there's no
			 * clear way for a caller to respond to that, so we forbid
			 * recursive broadcasts outright.)
			 */
			err = EAGAIN;
			goto error;
		case 3: /* add_filter */
		case 4: /* rm_filter */
		{
			/* single inline-encoded sequence, so length is tag.X.u - 2. (it
			 * even consumes at most six bits, twee shit)
			 */
			int n_labels = L4_UntypedWords(tag) - 2;
			L4_Word_t mask, labels[60];
			L4_StoreMR(2, &mask);
			L4_StoreMRs(3, n_labels, labels);
			if(oplabel == 3) add_filter(sender, mask, labels, n_labels);
			else rm_filter(sender, mask, labels, n_labels);
			L4_LoadMR(0, 0);
			break;
		}
		default:
			log_info("unexpected recursive tag=%#lx", tag.raw);
			err = ENOSYS;
			goto error;
	}
reply:	/* "ReplyReceive" */
	L4_Accept(L4_UntypedWordsAcceptor);
	return L4_Ipc(sender, sender, L4_Timeouts(L4_ZeroTime, L4_Never), &dummy);
error:
	L4_LoadMR(0, (L4_MsgTag_t){ .X.label = 1, .X.u = 1 }.raw);
	L4_LoadMR(1, err);
	goto reply;
}

int main(void)
{
	main_tid = L4_Myself();
	/* siphash is a bit much for this, but w/e until sys/crt gets random(). */
	static const unsigned char t_key[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
	uint64_t salt = UINT64_C(0xBADC0FFEE0DDF00D);
	salt = siphash_2_4(&salt, sizeof salt, t_key);
	for(int i=0; i < ARRAY_SIZE(filter_salt); i++) {
		salt = siphash_2_4(&salt, sizeof salt, t_key);
		filter_salt[i] = salt;
	}
	static const struct sysmsg_vtable vtab = {
		.setmask = &impl_setmask,
		.broadcast = &impl_broadcast,
		.add_filter = &impl_add_filter,
		.rm_filter = &impl_rm_filter,
	};
	for(;;) {
		L4_Word_t status = _muidl_sysmsg_dispatch(&vtab);
		if(status != 0 && !MUIDL_IS_L4_ERROR(status) && selftest_handling(status)) {
			/* oof */
		} else {
			log_info("dispatch status %#lx (last tag %#lx)", status, muidl_get_tag().raw);
		}
	}
	return 0;
}
