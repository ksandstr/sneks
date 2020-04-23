
#define SYSMSG_IMPL_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <ccan/darray/darray.h>
#include <ccan/array_size/array_size.h>
#include <ccan/htable/htable.h>
#include <ccan/siphash/siphash.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>

#include <sneks/hash.h>
#include <sneks/bitops.h>
#include <sneks/process.h>
#include <sneks/systask.h>

#include "muidl.h"
#include "msg-defs.h"


#define FILTER_SIZE (64 / sizeof(L4_Word_t))	/* one cacheline's worth */
#define SLOTS_PER_LIMB (sizeof(L4_Word_t) * 8 / 4)


struct msgclient
{
	L4_ThreadId_t recv_tid;
	int subs_mask;
	L4_Word_t *filter;	/* 4-bit counting 3-hash bloom filter */
};


static size_t rehash_msgclient(const void *ptr, void *priv);
static L4_MsgTag_t recursive_sysmsg_call(
	L4_MsgTag_t tag, L4_ThreadId_t sender);
static bool filter_pass(L4_Word_t *filter, int maskp, uint32_t hash);


static darray(struct msgclient *) subs[32];
static struct htable client_ht = HTABLE_INITIALIZER(client_ht,
	&rehash_msgclient, NULL);

static L4_ThreadId_t main_tid;
static L4_Word_t filter_salt[ARRAY_SIZE(subs)];


static size_t rehash_msgclient(const void *ptr, void *priv) {
	return int_hash(pidof_NP(((const struct msgclient *)ptr)->recv_tid));
}


static bool cmp_pid_to_msgclient(const void *cand, void *key) {
	const struct msgclient *c = cand;
	return pidof_NP(c->recv_tid) == *(int *)key;
}


static int order_msgclientptr_by_pid(const void *a, const void *b) {
	const struct msgclient *l = *(const struct msgclient **)a,
		*r = *(const struct msgclient **)b;
	return pidof_NP(l->recv_tid) - pidof_NP(r->recv_tid);
}


static struct msgclient *get_client(L4_ThreadId_t tid) {
	int pid = pidof_NP(tid);
	return htable_get(&client_ht, int_hash(pid), &cmp_pid_to_msgclient, &pid);
}


/* TODO: call this from whatever notifies about systask deaths. */
#if !1
static void msgclient_dtor(struct msgclient *c)
{
	assert(c->subs_mask == 0);
	bool ok = htable_del(&client_ht, rehash_msgclient(c, NULL), c);
	assert(ok);
	free(c->filter);
	free(c);
}
#endif


/* NOTE: this has two calls to abort() in it due to failing mallocs. ideally
 * it'd have an error return path instead, though it's not known what systasks
 * would do if Sysmsg::setmask fails.
 */
static int impl_setmask(int32_t or, int32_t and)
{
	L4_ThreadId_t recv_tid = muidl_get_sender();
	struct msgclient *c = get_client(recv_tid);
	if(c == NULL && and == 0 && or == 0) return 0;
	if(c == NULL) {
		c = malloc(sizeof *c);
		if(c == NULL) abort();
		*c = (struct msgclient){ .recv_tid = recv_tid };
		bool ok = htable_add(&client_ht, rehash_msgclient(c, NULL), c);
		if(!ok) abort();
	}

	int old_mask = c->subs_mask;
	c->subs_mask = (c->subs_mask & and) | or;
	int removed = old_mask & ~c->subs_mask, added = ~old_mask & c->subs_mask;
	//printf("%s: removed=%#x, added=%#x\n", __func__, removed, added);
	while(removed != 0) {
		int bit = ffsl(removed) - 1;
		assert(bit >= 0);
		assert(subs[bit].size > 0);
		removed &= ~(1 << bit);
		bool found = false;
		for(int i=0; i < subs[bit].size; i++) {
			if(subs[bit].item[i] == c) {
				subs[bit].item[i] = subs[bit].item[subs[bit].size - 1];
				darray_resize(subs[bit], subs[bit].size - 1);
				found = true;
				break;
			}
		}
		assert(found);
	}
	while(added != 0) {
		int bit = ffsl(added) - 1;
		assert(bit >= 0);
		added &= ~(1 << bit);
		darray_push(subs[bit], c);
	}

	return old_mask;
}


static int send_to_client(
	struct msgclient *c,
	L4_Word_t bits, const L4_Word_t *body, size_t body_len)
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
	 * from within a handler may cause the outer handler call to fail also,
	 * which may in turn cause handler replies to appear in the muidl dispatch
	 * loop.
	 */
	if(L4_IpcFailed(tag)) return -(int)L4_ErrorCode();

	assert(L4_UntypedWords(tag) == 1);
	L4_Word_t st; L4_StoreMR(1, &st);
	return st;
}


static bool impl_broadcast(
	int32_t maskp, int32_t maskn,
	const L4_Word_t *body, unsigned body_len)
{
	if(maskp == 0 || maskn == ~0) return true;	/* no-ops */

	/* collect receivers. */
	struct msgclient **receivers;
	int n_receivers;
	darray(struct msgclient *) temp = darray_new();
	bool merge = __builtin_popcount(maskp) > 1;
	if(!merge && maskn == 0) {
		int bit = ffsl(maskp) - 1;
		receivers = subs[bit].item;
		n_receivers = subs[bit].size;
	} else {
		int acc = maskp;
		while(acc != 0) {
			int bit = ffsl(acc) - 1;
			acc &= ~(1 << bit);
			if(maskn == 0) {
				darray_append_items(temp, subs[bit].item, subs[bit].size);
			} else {
				darray_realloc(temp, temp.size + subs[bit].size);
				for(int i=0; i < subs[bit].size; i++) {
					if((subs[bit].item[i]->subs_mask & maskn) == 0) {
						darray_push(temp, subs[bit].item[i]);
					}
				}
			}
		}
		/* sort, uniq */
		qsort(temp.item, temp.size, sizeof temp.item[0],
			&order_msgclientptr_by_pid);
		n_receivers = 0;
		for(int i=1; i < temp.size; i++) {
			if(order_msgclientptr_by_pid(&temp.item[i-1],
				&temp.item[i]) != 0)
			{
				temp.item[n_receivers++] = temp.item[i];
			}
		}
		receivers = temp.item;
	}

	bool immediate = true;
	uint32_t hash = int_hash(body[0]);
	for(int i=0; i < n_receivers; i++) {
		assert(i < 1 || receivers[i - 1] != receivers[i]);
		if(!filter_pass(receivers[i]->filter, maskp, hash)) continue;
		int n = send_to_client(receivers[i], maskp, body, body_len);
		if(n == 1) immediate = false;
		else if(n < 0) {
			printf("sysmsg: couldn't send to pid=%d, n=%d\n",
				pidof_NP(receivers[i]->recv_tid), n);
		}
	}

	darray_free(temp);
	return immediate;
}


/* four-bit unsigned saturating increment.
 *
 * returns true when the field became saturated, false if it was already or is
 * afterward at non-saturation. each field is 4 bits wide for maximum
 * reversibility.
 */
static bool filter_inc(L4_Word_t *filter, int slot)
{
	int limb = slot / SLOTS_PER_LIMB, offs = (slot % SLOTS_PER_LIMB) * 4;
#if 1
	bool sat = ((filter[limb] >> offs) & 0xf) == 0xe;
	if(((filter[limb] >> offs) & 0xf) < 0xf) filter[limb] += 1lu << offs;
	return sat;
#else
	L4_Word_t mask = 0xf << offs, val = filter[limb] & mask,
		notsat = MASK32(val - mask);
	filter[limb] = (~notsat & filter[limb])
		| (notsat & (filter[limb] + (1 << offs)));

	return notsat && ((filter[limb] & mask) == mask);
#endif
}


/* four-bit unsigned saturation-preserving decrement. also preserves zeroes,
 * even though the sole call site's use of filter_query() ensures they never
 * appear.
 */
static void filter_dec(L4_Word_t *filter, int slot)
{
	int limb = slot / SLOTS_PER_LIMB, offs = (slot % SLOTS_PER_LIMB) * 4;
#if 1
	if((((filter[limb] >> offs) + 1) & 0xf) > 1) filter[limb] -= 1lu << offs;
#else
	L4_Word_t mask = 0xf << offs, val = filter[limb] & mask,
		notsat = MASK32(val - mask), notzero = MASK32(val);
	filter[limb] = ((~notsat | ~notzero) & filter[limb])
		| (notsat & notzero & (filter[limb] - (1 << offs)));
#endif
}


static void get_slots(int slots[static 3], uint32_t hash)
{
	const int n_slots = FILTER_SIZE * sizeof(L4_Word_t) * 8 / 4;
	for(int i=0; i < 3; i++) {
		slots[i] = hash & (n_slots - 1);
		hash /= n_slots;
	}
	/* FIXME: make slots[0..2] distinct. */
}


static bool filter_query(L4_Word_t *filter, const int slots[static 3])
{
	bool ret = true;
	for(int i=0; i < 3; i++) {
		int limb = slots[i] / SLOTS_PER_LIMB,
			offs = (slots[i] % SLOTS_PER_LIMB) * 4;
		ret &= !(filter[limb] & (0xf << offs));
	}
	return ret;
}


static bool filter_pass(L4_Word_t *filter, int maskp, uint32_t hash)
{
	if(filter == NULL) return true;
	for(int acc = maskp, bit = ffsl(acc) - 1;
		acc != 0;
		acc &= ~(1 << bit), bit = ffsl(acc) - 1)
	{
		int slots[3]; get_slots(slots, hash ^ filter_salt[bit]);
		if(filter_query(filter, slots)) return true;
	}
	return false;
}


#ifdef BUILD_SELFTEST

#include <sneks/test.h>
#include <sneks/systask.h>


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


static void impl_add_filter(
	int mask, const L4_Word_t *labels, unsigned n_labels)
{
	struct msgclient *c = get_client(muidl_get_sender());
	if(c == NULL) return;	/* do setmask first plz */
	if(c->filter == NULL) {
		c->filter = calloc(FILTER_SIZE, sizeof *c->filter);
		if(c->filter == NULL) {
			fprintf(stderr, "can't allocate client filter!\n");
			/* FIXME: set some kind of an all-saturated filter, and don't free
			 * it in msgclient_dtor()?
			 */
			return;
		}
	}

	for(int i=0; i < n_labels; i++) {
		uint32_t hash = int_hash(labels[i]);
		for(int acc = mask, bit = ffsl(acc) - 1;
			acc != 0;
			acc &= ~(1 << bit), bit = ffsl(acc) - 1)
		{
			int slots[3];
			get_slots(slots, hash ^ filter_salt[bit]);
			for(int j=0; j < 3; j++) filter_inc(c->filter, slots[j]);
		}
	}
}


static void impl_rm_filter(
	int mask, const L4_Word_t *labels, unsigned n_labels)
{
	struct msgclient *c = get_client(muidl_get_sender());
	if(c == NULL || c->filter == NULL) return;

	for(int i=0; i < n_labels; i++) {
		uint32_t hash = int_hash(labels[i]);
		for(int acc = mask, bit = ffsl(acc) - 1;
			acc != 0;
			acc &= ~(1 << bit), bit = ffsl(acc) - 1)
		{
			int slots[3];
			get_slots(slots, hash ^ filter_salt[bit]);
			if(!filter_query(c->filter, slots)) continue;
			for(int j=0; j < 3; j++) filter_dec(c->filter, slots[j]);
		}
	}
}


static L4_MsgTag_t recursive_sysmsg_call(
	L4_MsgTag_t tag, L4_ThreadId_t sender)
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
			int oldmask = impl_setmask(or, and);
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
			if(oplabel == 3) impl_add_filter(mask, labels, n_labels);
			else impl_rm_filter(mask, labels, n_labels);
			L4_LoadMR(0, 0);
			break;
		}
		default:
			printf("sysmsg: weird recursive tag=%#lx\n", tag.raw);
			err = ENOSYS;
			goto error;
	}

reply:
	/* "ReplyReceive" */
	L4_Accept(L4_UntypedWordsAcceptor);
	return L4_Ipc(sender, sender,
		L4_Timeouts(L4_ZeroTime, L4_Never), &dummy);

error:
	L4_LoadMR(0, (L4_MsgTag_t){ .X.label = 1, .X.u = 1 }.raw);
	L4_LoadMR(1, err);
	goto reply;
}


int main(void)
{
	main_tid = L4_Myself();
	for(int i=0; i < ARRAY_SIZE(subs); i++) darray_init(subs[i]);

	/* siphash is a bit much for this, but w/e until sys/crt gets random(). */
	static const unsigned char t_key[16] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
	};
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
		if(status != 0 && !MUIDL_IS_L4_ERROR(status)
			&& selftest_handling(status))
		{
			/* oof */
		} else {
			fprintf(stderr, "sysmsg: dispatch status %#lx (last tag %#lx)\n",
				status, muidl_get_tag().raw);
		}
	}

	return 0;
}
