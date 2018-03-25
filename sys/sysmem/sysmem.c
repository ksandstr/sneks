
#define SYSMEMIMPL_IMPL_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdalign.h>
#include <string.h>
#include <errno.h>
#include <ccan/list/list.h>
#include <ccan/likely/likely.h>
#include <ccan/minmax/minmax.h>
#include <ccan/array_size/array_size.h>
#include <ccan/container_of/container_of.h>

#include <sneks/rbtree.h>
#include <sneks/mm.h>
#include <sneks/bitops.h>
#include <sneks/process.h>
#include <sneks/rootserv.h>
#include <sneks/lz4.h>

#include <l4/types.h>
#include <l4/kip.h>
#include <l4/ipc.h>
#include <l4/space.h>
#include <l4/thread.h>
#include <l4/kdebug.h>

#include "muidl.h"
#include "info-defs.h"
#include "abend-defs.h"
#include "sysmem-impl-defs.h"


#define NUM_INIT_PAGES 12	/* traditionally enough. */
#define C_MAGIC 0x740b1d8a
#define PROXY_ABEND_LABEL 0xbaa6

#define LF_SWAP 1	/* p_addr{12..31} designates a c_page. */

/* can't usefully store compressed pages larger than a size that'd leave less
 * space over than what the minimum compressed result would use.
 */
#define MAX_COMPRESSED_SIZE (PAGE_SIZE - sizeof(struct c_hdr) - 26)



struct l_page;

/* physical memory */
struct p_page
{
	/* member of one or zero of free_page_list, active_page_list, or an
	 * alloc_head.pages.
	 */
	struct list_node link;
	L4_Word_t address;
	struct l_page *owner;	/* single owner iff mapped, otherwise NULL */
	int age;
};


/* logical memory.
 * TODO: this has a hideous per-page overhead but it'll do for now. the
 * rb-tree is used to find the associated systask so it's not all bad.
 */
struct l_page
{
	struct rb_node rb;	/* in systask.mem */
	L4_Word_t l_addr;	/* address in task */
	/* bits 0..11 are a mask of LF_*, flags of the logical memory.
	 * bits 12..31 indicate syspager address of the physical page. depending
	 * on flags this may be storage for the compressed contents of the virtual
	 * memory page.
	 */
	L4_Word_t p_addr;
};


/* storage header for pages containing compressed memory. */
struct c_hdr
{
	uint32_t magic;	/* C_MAGIC */
	struct p_page *phys;
	struct list_node buddy_link;
	uint16_t a_len, b_len;	/* compressed content length */
	struct l_page *a, *b;
};


struct systask
{
	struct rb_node rb;		/* in systask_tree by ->pid asc */
	struct rb_root mem;		/* of <struct l_page> via rb */
	uintptr_t brk;			/* program break (inclusive) */
	uintptr_t min_fault;
	L4_Fpage_t utcb_area, kip_area;	/* FIXME: fill these in */
	uint16_t pid;			/* systask ID per pidof_NP() */
};


/* per-type allocation context. supports eager trash-stack recycling. */
struct alloc_head
{
	short sz, align;
	struct list_head pages;	/* of p_page. current alloc page at head. */
	void *trash;			/* address of first free item */
};


/* inline per-page header. */
struct alloc_page {
	void *next;		/* next == NULL means no more fresh allocs */
};


/* note that this leaves .pages NULL, requiring initialization in alloc(). */
#define ALLOC_HEAD(typ) ((struct alloc_head){ \
	.sz = sizeof(typ), .align = alignof(typ), \
	.trash = NULL })


L4_KernelInterfacePage_t *the_kip;

static struct list_head free_page_list = LIST_HEAD_INIT(free_page_list),
	active_page_list = LIST_HEAD_INIT(active_page_list),
	reserved_page_list = LIST_HEAD_INIT(reserved_page_list);
static unsigned num_free_pages = 0;

static bool repl_enable = false;

static struct alloc_head systask_alloc_head = ALLOC_HEAD(struct systask),
	p_page_alloc_head = ALLOC_HEAD(struct p_page),
	l_page_alloc_head = ALLOC_HEAD(struct l_page);

static struct rb_root systask_tree = RB_ROOT;

static uint8_t first_mem[PAGE_SIZE * NUM_INIT_PAGES]
	__attribute__((aligned(PAGE_SIZE)));

static L4_ThreadId_t abend_helper_tid;

/* sysinfo data. should be moved into a different module so as to not make
 * this one even biggar.
 */
static struct sneks_kmsg_info kmsg_info;
static struct sneks_abend_info abend_info;
static struct sneks_uapi_info uapi_info;


static struct p_page *get_free_page(void);


/* runtime basics. */

static void abend(long class, const char *fmt, ...)
{
	va_list al;
	va_start(al, fmt);
	static char fail[300];
	vsnprintf(fail, sizeof fail, fmt, al);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.label = PROXY_ABEND_LABEL, .X.u = 2 }.raw);
	L4_LoadMR(1, class);
	L4_LoadMR(2, (L4_Word_t)&fail[0]);
	L4_MsgTag_t tag = L4_Send(abend_helper_tid);
	if(L4_IpcFailed(tag)) {
		printf("%s: IPC to abend_helper failed, ec=%#lx\n",
			__func__, L4_ErrorCode());
	}
}


void abort(void) {
	printf("sysmem: abort() called from %p!\n",
		__builtin_return_address(0));
	for(;;) L4_Sleep(L4_Never);
}


NORETURN void panic(const char *msg) {
	printf("sysmem: PANIC! %s\n", msg);
	abort();
}


void con_putstr(const char *string) {
	L4_KDB_PrintString((char *)string);
}


void __assert_failure(
	const char *cond, const char *file, unsigned int line, const char *fn)
{
	printf("!!! assert(%s) failed in `%s' [%s:%u]\n", cond, fn, file, line);
	abort();
}


/* data structure manipulation. */

static struct systask *find_task(int pid)
{
	if(pid < SNEKS_MIN_SYSID) return NULL;

	struct rb_node *n = systask_tree.rb_node;
	while(n != NULL) {
		struct systask *t = rb_entry(n, struct systask, rb);
		if(pid < t->pid) n = n->rb_left;
		else if(pid > t->pid) n = n->rb_right;
		else return t;
	}
	return NULL;
}


static struct systask *put_task_helper(struct systask *t)
{
	struct rb_node **p = &systask_tree.rb_node, *parent = NULL;
	while(*p != NULL) {
		parent = *p;
		struct systask *oth = rb_entry(parent, struct systask, rb);
		if(t->pid < oth->pid) p = &(*p)->rb_left;
		else if(t->pid > oth->pid) p = &(*p)->rb_right;
		else return oth;
	}

	rb_link_node(&t->rb, parent, p);
	return NULL;
}


static void put_task(struct systask *t)
{
	struct systask *dupe = put_task_helper(t);
	if(dupe != NULL) panic("duplicate in put_task()!");
	rb_insert_color(&t->rb, &systask_tree);
	assert(find_task(t->pid) == t);
}


static inline struct l_page *put_lpage_helper(
	struct systask *task, struct l_page *lp)
{
	struct rb_node **p = &task->mem.rb_node, *parent = NULL;
	while(*p != NULL) {
		parent = *p;
		struct l_page *oth = rb_entry(parent, struct l_page, rb);
		if(lp->l_addr < oth->l_addr) p = &(*p)->rb_left;
		else if(lp->l_addr > oth->l_addr) p = &(*p)->rb_right;
		else return oth;
	}

	rb_link_node(&lp->rb, parent, p);
	return NULL;
}


static void put_lpage(struct systask *task, struct l_page *lp)
{
	struct l_page *dupe = put_lpage_helper(task, lp);
	if(dupe != NULL) panic("duplicate in put_lpage()!");
	rb_insert_color(&lp->rb, &task->mem);
}


static struct l_page *get_lpage(struct systask *task, L4_Word_t addr)
{
	assert((addr & PAGE_MASK) == 0);
	struct rb_node *n = task->mem.rb_node;
	while(n != NULL) {
		struct l_page *cand = rb_entry(n, struct l_page, rb);
		L4_Word_t cand_addr = cand->l_addr & ~PAGE_MASK;
		if(addr < cand_addr) n = n->rb_left;
		else if(addr > cand_addr) n = n->rb_right;
		else return cand;
	}
	return NULL;
}


/* hideously inefficient, but there's no real data structure for tracking
 * these things for now.
 */
static struct p_page *get_active_page(L4_Word_t addr)
{
	struct p_page *p;
	list_for_each(&active_page_list, p, link) {
		if(p->address == addr) return p;
	}
	return NULL;
}


/* malloc and free using a tiny static pool of memory.
 *
 * TODO: this is super braindead. replace with the worst babbymode allocator
 * from CCAN.
 */
static uint8_t malloc_pool[1 * 1024] __attribute__((aligned(4096)));

void *malloc(size_t sz)
{
	static size_t pos = 0;
	size_t orig_sz = sz;
	sz = (sz + 7) & ~7;
	if(pos + sz >= sizeof malloc_pool) {
		printf("sysmem: malloc_pool can't satisfy sz=%u at pos=%u\n",
			(unsigned)orig_sz, (unsigned)pos);
		return NULL;
	}
	void *ptr = &malloc_pool[pos];
	pos += sz;
	return ptr;
}


void *calloc(size_t nmemb, size_t size) {
	void *ptr = malloc(nmemb * size);
	if(ptr != NULL) memset(ptr, 0, nmemb * size);
	return ptr;
}


void free(void *ptr) {
	printf("sysmem: tried to free(ptr=%p); does nothing\n", ptr);
}


/* sub-page memory allocation and recycling. */

#define alloc_struct(name) (struct name *)alloc(&name ## _alloc_head)
#define recycle_struct(typ, ptr) recycle(&typ ## _alloc_head, (ptr))

static void *alloc(struct alloc_head *head)
{
	/* recycling fastpath */
	if(head->trash != NULL) {
		void *ptr = head->trash;
		head->trash = *(void **)ptr;
		return ptr;
	}

	if(unlikely(head->pages.n.next == NULL)) {
		/* head init */
		list_head_init(&head->pages);
		assert(head->pages.n.next != NULL);
		goto get_ram;
	}

	struct p_page *phys = list_top(&head->pages, struct p_page, link);
	assert(phys != NULL);
	struct alloc_page *p = (struct alloc_page *)phys->address;
	if(p->next == NULL) {
get_ram:
		/* get more ram. */
		phys = get_free_page();
		list_add(&head->pages, &phys->link);
		p = (struct alloc_page *)phys->address;
		p->next = (void *)phys->address
			+ ((sizeof *p + head->align - 1) & ~(head->align - 1));
	}
	void *ptr = p->next;
	p->next += (head->sz + head->align - 1) & ~(head->align - 1);
	if(p->next >= (void *)phys->address + PAGE_SIZE) goto get_ram;

	return ptr;
}


/* (can't call it free().) */
static void recycle(struct alloc_head *head, void *ptr)
{
	memset(ptr + sizeof(void *), 0xde, head->sz - sizeof(void *));
	*(void **)ptr = head->trash;
	head->trash = ptr;
}


/* page replacement using a basic clock algorithm and Linux-style aging,
 * backed by memory compression using LZ4.
 */

static char lz4_state[LZ4_STREAMSIZE] __attribute__((aligned(8))); /* hueg */
static struct list_head buddy_list = LIST_HEAD_INIT(buddy_list);

static bool replace_active_page(struct p_page *p)
{
	L4_Fpage_t fp = L4_FpageLog2(p->address, PAGE_BITS);
	L4_Set_Rights(&fp, L4_FullyAccessible);
	L4_UnmapFpage(fp);

	assert(p->owner != NULL);
	char outbuf[LZ4_COMPRESSBOUND(PAGE_SIZE)];
	int n = LZ4_compress_fast_extState(lz4_state, (void *)p->address,
		outbuf, PAGE_SIZE, sizeof outbuf, 1);
	if(n == 0 || n >= MAX_COMPRESSED_SIZE) {
		printf("sysmem: page %#lx wouldn't compress (n=%d)\n",
			p->address, n);
		return false;
	}
	int c_size = n;
	// printf("sysmem: page %#lx compressed to %d bytes\n", p->address, c_size);

	/* find the closest matching buddy, i.e. one that causes the least waste
	 * compared to storing the replaced page in itself.
	 * TODO: optimize this to not be a silly brute force loop.
	 */
	struct c_hdr *best = NULL, *cand;
	int waste = PAGE_SIZE - sizeof(struct c_hdr) - c_size;
	list_for_each(&buddy_list, cand, buddy_link) {
		assert(cand->magic == C_MAGIC);
		int left = PAGE_SIZE - sizeof(struct c_hdr)
			- (cand->a != NULL ? cand->a_len : cand->b_len);
		if(left < c_size) continue;
		if(left - c_size < waste) {
			waste = left - c_size;
			best = cand;
		}
	}

	list_del_from(&active_page_list, &p->link);
	if(best == NULL) {
		// printf("sysmem: turning it into storage w/ %d bytes left\n", waste);
		best = (void *)p->address;
		*best = (struct c_hdr){
			.magic = C_MAGIC, .phys = p,
			.a_len = c_size, .a = p->owner,
		};
		memcpy(&best[1], outbuf, c_size);
		list_add(&buddy_list, &best->buddy_link);
		p->owner->p_addr |= LF_SWAP;
		p->owner = NULL;
	} else {
		// printf("sysmem: storing into existing buddy, wasting %d bytes\n", waste);
		list_del_from(&buddy_list, &best->buddy_link);
		void *addr;
		if(best->a == NULL) {
			addr = &best[1];
			best->a = p->owner;
			best->a_len = c_size;
		} else {
			addr = (void *)best + PAGE_SIZE - c_size;
			assert(addr >= (void *)&best[1]);
			best->b = p->owner;
			best->b_len = c_size;
		}
		memcpy(addr, outbuf, c_size);

		p->owner->p_addr = ((L4_Word_t)best & ~PAGE_MASK) | LF_SWAP;
		p->owner = NULL;
		list_add(&free_page_list, &p->link);
		num_free_pages++;
	}

	return true;
}


/* TODO: add proper termination so that a proper and honest OOM doesn't
 * turn into a perpetual spin.
 */
static int replace_pages(void)
{
	static struct p_page *hand = NULL;
	if(hand == NULL || hand->owner == NULL) {
		hand = list_top(&active_page_list, struct p_page, link);
	}

	/* examine one Unmap syscall's worth of pages per call. */
	L4_Fpage_t unmaps[64];
	struct p_page *ps[ARRAY_SIZE(unmaps)];
	int n_seen = 0;
	struct p_page *start = hand;
	do {
		if(hand == NULL && list_empty(&active_page_list)) {
			panic("active_page_list was drained. woe is me");
		}
		assert(hand != NULL);
		assert(hand->owner != NULL);
		ps[n_seen] = hand;
		unmaps[n_seen] = L4_FpageLog2(hand->address, PAGE_BITS);
		L4_Set_Rights(&unmaps[n_seen], 0);
		hand = list_next(&active_page_list, hand, link);
		if(hand == NULL) {
			hand = list_top(&active_page_list, struct p_page, link);
		}
	} while(++n_seen < ARRAY_SIZE(unmaps) && start != hand);

	L4_UnmapFpages(ARRAY_SIZE(unmaps), unmaps);
	int n_freed = 0;
	for(int i=0; i < n_seen; i++) {
		if(L4_Rights(unmaps[i]) != 0) ps[i]->age++;
		else if(ps[i]->age > 0) ps[i]->age >>= 1;
		else {
			/* actual replacement. */
			struct p_page *next = list_next(&active_page_list, ps[i], link);
			if(replace_active_page(ps[i])) {
				if(ps[i] == hand) {
					hand = next != NULL ? next
						: list_top(&active_page_list, struct p_page, link);
				}
				n_freed++;
			}
		}
	}

	return n_freed;
}


static struct p_page *get_free_page(void)
{
	struct p_page *p = list_pop(&free_page_list, struct p_page, link);
	if(p == NULL) panic("out of memory in get_free_page()!");
	num_free_pages--;
	p->age = 1;

	/* static low and high watermarks. */
	if(repl_enable && num_free_pages < 16) {
		while(num_free_pages < 48) {
#if 0
			printf("sysmem: running replacement (nfp=%u)\n",
				(unsigned)num_free_pages);
#endif
			int n_freed = replace_pages();
			if(n_freed > 0) printf("sysmem: replaced %d pages\n", n_freed);
		}
	}

	return p;
}


static struct p_page *decompress(struct l_page *lp)
{
	struct c_hdr *ch = (struct c_hdr *)(lp->p_addr & ~PAGE_MASK);
	assert(ch->magic == C_MAGIC);
	void *data;
	int len;
	bool done;
	if(ch->a == lp) {
		data = &ch[1];
		len = ch->a_len;
		ch->a = NULL;
		done = ch->b == NULL;
	} else {
		assert(ch->b == lp);
		data = (void *)ch + PAGE_SIZE - ch->b_len;
		len = ch->b_len;
		ch->b = NULL;
		done = ch->a == NULL;
	}
	struct p_page *phys;
	int n;
	if(!done) {
		phys = get_free_page();
		list_add(&buddy_list, &ch->buddy_link);
		n = LZ4_decompress_safe(data, (void *)phys->address, len, PAGE_SIZE);
	} else {
		/* expand into the same page via a buffer. */
		list_del_from(&buddy_list, &ch->buddy_link);
		phys = ch->phys;
		char buf[len];
		memcpy(buf, data, len);
		n = LZ4_decompress_safe(buf, (void *)phys->address, len, PAGE_SIZE);
	}
	if(n != PAGE_SIZE) {
		printf("%s: n=%d\n", __func__, n);
		panic("invalid decompressed length!");
	}

	return phys;
}


/* constructs systasks as they appear. could return errptrs instead, there's a
 * few different kinds of fail in there.
 */
static struct systask *get_task(int pid)
{
	assert(pid >= SNEKS_MIN_SYSID);
	struct systask *t = find_task(pid);
	if(t == NULL) {
		t = alloc_struct(systask);
		t->mem = RB_ROOT;
		t->brk = 0x10000;	/* 64k should be enough for everyone */
		t->min_fault = ~PAGE_MASK;
		t->pid = pid;
		t->kip_area = t->utcb_area = L4_Nilpage;
		put_task(t);
	}

	return t;
}


/* pagefault handling. this supports two use cases; normal fault service via
 * impl_handle_fault(), and in-band faults happening during interactions with
 * systasks such as may arise in e.g. threadctl().
 */
static bool handle_pf(
	L4_MapItem_t *page_ptr,
	L4_ThreadId_t tid, L4_Word_t faddr, L4_Word_t fip)
{
#if 0
	printf("pf in %lu:%lu, faddr=%#lx, fip=%#lx\n",
		L4_ThreadNo(tid), L4_Version(tid), faddr, fip);
#endif

	if((faddr & ~PAGE_MASK) == 0) {
		printf("sysmem: zero page access in %lu:%lu, fip=%#lx\n",
			L4_ThreadNo(tid), L4_Version(tid), fip);
		return false;
	}

	struct systask *task = get_task(pidof_NP(tid));
	if(task == NULL) {
		if(pidof_NP(tid) < SNEKS_MIN_SYSID) {
			printf("sysmem: illegal pf from non-systask tid=%lu:%lu (pid=%u)\n",
				L4_ThreadNo(tid), L4_Version(tid), pidof_NP(tid));
		} else {
			printf("sysmem: unknown tid=%lu:%lu (pid=%u)\n",
				L4_ThreadNo(tid), L4_Version(tid), pidof_NP(tid));
		}
		return false;
	}

	struct l_page *lp = get_lpage(task, faddr & ~PAGE_MASK);
	if(lp == NULL && (faddr & ~PAGE_MASK) > task->brk) {
		abend(PANIC_EXIT | PANICF_SEGV,
			"systask=%lu:%lu segv; faddr=%#lx fip=%#lx\n",
			L4_ThreadNo(tid), L4_Version(tid), faddr, fip);
		return false;
	} else if(lp == NULL) {
		lp = alloc_struct(l_page);
		struct p_page *phys = get_free_page();
		lp->p_addr = phys->address;
		lp->l_addr = faddr & ~PAGE_MASK;
		phys->owner = lp;
		put_lpage(task, lp);
		list_add_tail(&active_page_list, &phys->link);
	} else if(lp->p_addr & LF_SWAP) {
		struct p_page *phys = decompress(lp);
		lp->p_addr = phys->address;
		phys->owner = lp;
		list_add_tail(&active_page_list, &phys->link);
	}

	task->min_fault = min_t(L4_Word_t, task->min_fault, faddr & ~PAGE_MASK);
	L4_Fpage_t fp = L4_FpageLog2(lp->p_addr & ~PAGE_MASK, PAGE_BITS);
	L4_Set_Rights(&fp, L4_FullyAccessible);
	*page_ptr = L4_MapItem(fp, faddr & ~PAGE_MASK);
	return true;
}


/* implementation of sneks::Sysmem */

static void impl_handle_fault(
	L4_Word_t faddr, L4_Word_t fip, L4_MapItem_t *page_ptr)
{
	if(!handle_pf(page_ptr, muidl_get_sender(), faddr, fip)) {
		muidl_raise_no_reply();
	}
}


/* see comment for handle_pf(). */
static bool interim_fault(L4_MsgTag_t tag, L4_ThreadId_t peer)
{
	if(L4_IpcFailed(tag) || tag.X.label >> 4 != 0xffe
		|| tag.X.u != 2 || tag.X.t != 0)
	{
		return false;
	}

	L4_MapItem_t map;
	L4_Word_t faddr, fip;
	L4_StoreMR(1, &faddr); L4_StoreMR(2, &fip);
	L4_Acceptor_t acc = L4_Accepted();
	printf("sysmem: interim fault addr=%#lx, ip=%#lx\n", faddr, fip);
	if(!handle_pf(&map, peer, faddr, fip)) {
		printf("%s: pager segfault!\n", __func__);
		abort();
	}
	L4_Accept(acc);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.t = 2 }.raw);
	L4_LoadMRs(1, 2, map.raw);
	return true;
}


static int threadctl(
	L4_ThreadId_t dest,
	L4_ThreadId_t spacespec, L4_ThreadId_t sched, L4_ThreadId_t pager,
	void *utcb)
{
	L4_ThreadId_t peer = L4_Pager();
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.label = 0xb1c4, .X.u = 5 }.raw);
	L4_LoadMR(1, dest.raw);
	L4_LoadMR(2, spacespec.raw);
	L4_LoadMR(3, sched.raw);
	L4_LoadMR(4, pager.raw);
	L4_LoadMR(5, (L4_Word_t)utcb);
	L4_MsgTag_t tag = L4_Call(peer);
	while(interim_fault(tag, peer)) tag = L4_Call(peer);
	int n;
	if(L4_IpcFailed(tag)) {
		printf("%s: IPC failed, ec=%#lx\n", __func__, L4_ErrorCode());
		n = -(int)L4_ErrorCode();
	} else {
		L4_Word_t res = 0;
		L4_StoreMR(1, &res);
		n = res;
	}
	return n;
}


static void abend_helper_thread(void)
{
	for(;;) {
		L4_ThreadId_t sender;
		L4_MsgTag_t tag = L4_Ipc(L4_nilthread, L4_anylocalthread,
			L4_Timeouts(L4_Never, L4_Never), &sender);
		if(L4_IpcFailed(tag) || L4_Label(tag) != PROXY_ABEND_LABEL) {
			printf("sysmem: %s: ipc failed, tag=%#lx, ec=%#lx\n",
				__func__, tag.raw, L4_ErrorCode());
			continue;
		}

		L4_Word_t cls, ptr;
		L4_StoreMR(1, &cls);
		L4_StoreMR(2, &ptr);
		const char *fail = (void *)ptr;
		if(abend_info.service != 0) {
			L4_ThreadId_t serv = { .raw = abend_info.service };
			int n = __abend_long_panic(serv, cls, fail);
			printf("sysmem: Abend::long_panic() returned n=%d\n", n);
		} else {
			printf("[Sneks::Abend not available! fail=`%s']\n", fail);
		}
	}
}


static void start_abend_helper(void)
{
	L4_ThreadId_t self = L4_Myself();
	abend_helper_tid = L4_GlobalId(L4_ThreadNo(self) + 1, L4_Version(self));
	uintptr_t utcb_base = L4_MyLocalId().raw & ~511;
	int n = threadctl(abend_helper_tid, self, self, self,
		(void *)utcb_base + 512);
	if(n != 0) {
		printf("%s: threadctl failed, n=%d\n", __func__, n);
		abort();
	}

	struct p_page *stkpage = get_free_page();
	L4_Start_SpIp(abend_helper_tid, stkpage->address + PAGE_SIZE - 16,
		(L4_Word_t)&abend_helper_thread);
}


static void impl_rm_task(int32_t pid)
{
	printf("%s: pid=%d\n", __func__, pid);
	if(pid < SNEKS_MIN_SYSID) return;
	struct systask *t = find_task(pid);
	if(t == NULL) return;

	L4_Fpage_t fp[64];
	int n_fp = 0;
	for(struct rb_node *cur = rb_first(&t->mem), *next; cur != NULL; cur = next) {
		next = rb_next(cur);

		struct l_page *lp = rb_entry(cur, struct l_page, rb);
		struct p_page *p = get_active_page(lp->p_addr & ~PAGE_MASK);
		if(p == NULL) {
			printf("sysmem: rm_task: can't find phys page for log=%#lx?\n",
				lp->l_addr & ~PAGE_MASK);
			continue;
		}

		fp[n_fp] = L4_FpageLog2(p->address, PAGE_BITS);
		L4_Set_Rights(&fp[n_fp], L4_FullyAccessible);
		if(++n_fp == ARRAY_SIZE(fp)) {
			L4_UnmapFpages(ARRAY_SIZE(fp), fp);
			n_fp = 0;
		}
		list_del_from(&active_page_list, &p->link);
		list_add(&free_page_list, &p->link);
		rb_erase(&lp->rb, &t->mem);
		recycle_struct(l_page, lp);
	}
	if(n_fp > 0) L4_UnmapFpages(n_fp, fp);

	rb_erase(&t->rb, &systask_tree);
	recycle_struct(systask, t);
}


static void impl_send_phys(
	L4_Word_t dest_raw, L4_Word_t frame_num, int size_log2)
{
	L4_ThreadId_t dest_tid = { .raw = dest_raw };
	assert(L4_IsNilThread(dest_tid) || L4_IsGlobalId(dest_tid));
	struct systask *task = NULL;
	if(!L4_IsNilThread(dest_tid)) {
		int dest_pid = pidof_NP(dest_tid);
		if(dest_pid < SNEKS_MIN_SYSID) {
			printf("%s: dest_pid=%d is a no-go\n", __func__, dest_pid);
			return;
		}
		task = get_task(dest_pid);
	}

	/* TODO: handle arbitrary sizes of page. this is unquestionably a good
	 * thing to have; it'll make very large initializations happen in the
	 * blink of an eye. but it's still for the "four gigs of physical RAM"
	 * series, way out there.
	 */
	if(size_log2 != PAGE_BITS) {
		printf("send_phys can't hack this size_log2\n");
		return;
	}

	struct p_page *pg = alloc_struct(p_page);
	pg->owner = NULL;
	L4_Word_t phys_addr = frame_num << PAGE_BITS;
	pg->address = phys_addr;

	L4_ThreadId_t sender = muidl_get_sender();
	L4_Accept(L4_MapGrantItems(L4_FpageLog2(phys_addr, size_log2)));
	L4_MsgTag_t tag = L4_Receive_Timeout(sender, L4_TimePeriod(5 * 1000));
	while(interim_fault(tag, sender)) {
		/* NOTE: this refreshes the timeout. */
		tag = L4_Call_Timeouts(sender, L4_ZeroTime, L4_TimePeriod(5 * 1000));
	}
	L4_Accept(L4_UntypedWordsAcceptor);
	if(L4_IpcFailed(tag)) {
		printf("failed send_phys second transaction, ec=%lu\n",
			L4_ErrorCode());
abnormal:
		recycle(&p_page_alloc_head, pg);
		return;
	} else if(L4_Label(tag) != 0 || L4_UntypedWords(tag) != 0
		|| L4_TypedWords(tag) != 2)
	{
		printf("out-of-band message in send_phys; tag=%#lx!\n", tag.raw);
		goto abnormal;
	}

	if(task == NULL) {
		/* "any old RAM" mode */
		struct list_head *list;
		/* recognize sysmem's own pages, add them to the reserved list. */
		extern char _start, _end;
		L4_Word_t top = ((L4_Word_t)&_end + PAGE_MASK) & ~PAGE_MASK;
		if(phys_addr >= (L4_Word_t)&_start && phys_addr < top) {
			/* exclude early memory from add_first_mem(), since it's already
			 * tracked.
			 */
			if(phys_addr >= (L4_Word_t)&first_mem[0]
				&& phys_addr < (L4_Word_t)&first_mem[sizeof first_mem])
			{
				list = NULL;
			} else {
				/* early memory (program image) not within first_mem[]. */
				list = &reserved_page_list;
			}
		} else {
			list = &free_page_list;
		}

		if(list != NULL) {
			list_add_tail(list, &pg->link);
			if(list == &free_page_list
				&& ++num_free_pages > 60 && !repl_enable)
			{
				repl_enable = true;
			}
		}
	} else {
		/* RAM for things loaded as boot modules. */
		struct l_page *lp = alloc_struct(l_page);
		lp->l_addr = phys_addr;
		lp->p_addr = phys_addr;
		put_lpage(task, lp);
		pg->owner = lp;
		pg->age = 1;
		list_add_tail(&active_page_list, &pg->link);
		task->min_fault = min_t(L4_Word_t, task->min_fault, phys_addr & ~PAGE_MASK);
		task->brk = max_t(L4_Word_t, task->brk, phys_addr | PAGE_MASK);
	}
}


static void unmap_page(L4_Word_t address)
{
	L4_Fpage_t p = L4_FpageLog2(address, PAGE_BITS);
	L4_Set_Rights(&p, L4_FullyAccessible);
	L4_UnmapFpage(p);
}


static uint16_t impl_send_virt(
	L4_Word_t src_addr, L4_Word_t dest_tid_raw, L4_Word_t dest_addr)
{
	int src_pid = pidof_NP(muidl_get_sender());
	if(src_pid < SNEKS_MIN_SYSID) {
		printf("%s: invalid src_pid=%d\n", __func__, src_pid);
		return EINVAL;
	}
	struct systask *src = get_task(src_pid);
	if(src == NULL) return ENOENT;

	struct l_page *lp = get_lpage(src, src_addr);
	if(lp == NULL) return EFAULT;

	L4_ThreadId_t dest_tid = { .raw = dest_tid_raw };
	if(!L4_IsNilThread(dest_tid)) {
		int dest_pid = pidof_NP(dest_tid);
		if(dest_pid < SNEKS_MIN_SYSID) {
			printf("%s: invalid dest_pid=%d\n", __func__, dest_pid);
			return EINVAL;
		}

		struct systask *dest = get_task(dest_pid);
		struct l_page *dstp = get_lpage(dest, dest_addr);
		if(dstp != NULL) {
			/* toss the previous page. */
			unmap_page(dstp->p_addr & ~PAGE_MASK);
			rb_erase(&dstp->rb, &dest->mem);
			/* FIXME: free the physical page! recycle `dstp'! */
		}

		/* move it over and unmap the physical memory. */
		unmap_page(lp->p_addr & ~PAGE_MASK);
		rb_erase(&lp->rb, &src->mem);
		lp->l_addr = dest_addr;
		put_lpage(dest, lp);
		dest->min_fault = min_t(L4_Word_t, dest->min_fault, dest_addr & ~PAGE_MASK);
		dest->brk = max_t(L4_Word_t, dest->brk, dest_addr | PAGE_MASK);
		return 0;
	} else if(L4_IsNilThread(dest_tid)) {
		/* just toss the page. */
		unmap_page(lp->p_addr & ~PAGE_MASK);
		rb_erase(&lp->rb, &src->mem);
		/* FIXME: release the physical memory! recycle `lp'! */
		return 0;
	} else {
		return ENOENT;
	}
}


/* TODO: could do unmaps in groups for overhead minimization: add a pointer to
 * 63 L4_Fpage_t and another to a fill counter.
 */
static void remove_l_page(struct systask *task, struct l_page *lp)
{
	if(lp->p_addr & LF_SWAP) {
		struct c_hdr *ch = (struct c_hdr *)(lp->p_addr & ~PAGE_MASK);
		assert(ch->magic == C_MAGIC);
		bool done;
		if(ch->a == lp) {
			done = ch->b == NULL;
			ch->a = NULL;
		} else {
			assert(ch->b == lp);
			done = ch->a == NULL;
			ch->b = NULL;
		}
		if(done) {
			/* implies membership in buddy list */
			list_del_from(&buddy_list, &ch->buddy_link);
			assert(ch->phys->owner == NULL);
			list_add(&free_page_list, &ch->phys->link);
			num_free_pages++;
		} else {
			/* otherwise, it should go in there. */
			list_add(&buddy_list, &ch->buddy_link);
		}
	} else {
		unmap_page(lp->p_addr & ~PAGE_MASK);
		struct p_page *phys = get_active_page(lp->p_addr & ~PAGE_MASK);
		assert(phys != NULL);
		assert(phys->owner == lp);
		list_del_from(&active_page_list, &phys->link);
		phys->owner = NULL;
		list_add(&free_page_list, &phys->link);
		num_free_pages++;
	}

	rb_erase(&lp->rb, &task->mem);
	recycle_struct(l_page, lp);
}


static void impl_brk(L4_Word_t new_brk)
{
	L4_ThreadId_t sender = muidl_get_sender();
	int pid = pidof_NP(sender);
	if(pid < SNEKS_MIN_SYSID) {
		printf("sysmem: %s: invalid pid=%d\n", __func__, pid);
		return;
	}
	struct systask *task = get_task(pid);
	if(task == NULL) {
		printf("sysmem: %s: no such task\n", __func__);
		return;
	}

	if(new_brk < task->brk) {
		/* release pages in range. */
		uintptr_t low = (new_brk + PAGE_MASK) & ~PAGE_MASK,
			high = task->brk | PAGE_MASK;
		/* FIXME: find the low point with an actual tree lookup. */
		struct l_page *cur = NULL;
		for(uintptr_t addr = low; addr < high && cur == NULL; addr += PAGE_SIZE) {
			cur = get_lpage(task, addr);
		}
		int n_dropped = 0;
		while(cur != NULL && cur->l_addr < high) {
			struct rb_node *next = rb_next(&cur->rb);
			remove_l_page(task, cur);
			cur = container_of_or_null(next, struct l_page, rb);
			n_dropped++;
		}
		printf("negative brk %#08lx -> %#08lx released %d pages\n",
			(L4_Word_t)task->brk, new_brk, n_dropped);
	}

	task->brk = new_brk;
	printf("brk for %lu:%lu set to %#lx\n",
		L4_ThreadNo(sender), L4_Version(sender), new_brk);
}


static unsigned short impl_breath_of_life(
	L4_Word_t tid_raw, L4_Word_t ip, L4_Word_t sp)
{
	L4_ThreadId_t tid = { .raw = tid_raw };
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 2 }.raw);
	L4_LoadMR(1, ip);
	L4_LoadMR(2, sp);
	L4_MsgTag_t tag = L4_Send_Timeout(tid, L4_TimePeriod(2 * 1000));
	return L4_IpcFailed(tag) ? L4_ErrorCode() : 0;
}


static int impl_get_shape(
	L4_Word_t *low_p, L4_Word_t *high_p,
	L4_Word_t task_raw)
{
	L4_ThreadId_t task = { .raw = task_raw };
	struct systask *t = find_task(pidof_NP(task));
	if(t == NULL) return ENOENT;

#define FP_EDGES(p) L4_Address((p)), L4_Address((p)) + L4_Size((p)) - 1
	L4_Word_t edges[] = {
		t->brk, t->min_fault,
		FP_EDGES(t->utcb_area),
		FP_EDGES(t->kip_area),
	};
#undef FP_EDGES

	*low_p = ~PAGE_MASK;
	*high_p = 0;
	for(int i=0; i < ARRAY_SIZE(edges); i++) {
		*low_p = min(*low_p, edges[i]);
		*high_p = max(*high_p, edges[i]);
	}

	return 0;
}


static int impl_lookup(L4_Word_t *info_tid) {
	*info_tid = L4_MyGlobalId().raw;
	return 0;
}


static int impl_kmsg_block(struct sneks_kmsg_info *kmsg) {
	*kmsg = kmsg_info;
	return 0;
}


static int impl_abend_block(struct sneks_abend_info *it) {
	*it = abend_info;
	return 0;
}


static int impl_uapi_block(struct sneks_uapi_info *it) {
	*it = uapi_info;
	return 0;
}


static void add_first_mem(void)
{
	for(int i=0; i < NUM_INIT_PAGES; i++) {
		struct p_page *phys = malloc(sizeof *phys);
		phys->address = (L4_Word_t)&first_mem[i * PAGE_SIZE];
		phys->owner = NULL;
		list_add_tail(&free_page_list, &phys->link);
		num_free_pages++;
	}
}


/* see lib/string.c comment for same fn */
static inline unsigned long haszero(unsigned long x) {
	return (x - 0x01010101ul) & ~x & 0x80808080ul;
}


static void sysinfo_init_msg(L4_MsgTag_t tag, const L4_Word_t mrs[static 64])
{
	/* decode the string part. */
	const int u = L4_UntypedWords(tag);
	char buffer[u * 4 + 1];
	int pos = 0;
	do {
		/* keep strict aliasing intact */
		memcpy(&buffer[pos * sizeof(L4_Word_t)],
			&mrs[pos], sizeof(L4_Word_t));
	} while(!haszero(mrs[pos++]) && pos < u);
	buffer[pos * sizeof(L4_Word_t)] = '\0';

	/* TODO: come up with a fancy method for setting this stuff. */
	const char *name = buffer;
	if(streq(name, "kmsg:tid")) {
		kmsg_info.service = mrs[pos++];
		assert(L4_IsGlobalId((L4_ThreadId_t){ .raw = kmsg_info.service }));
	} else if(streq(name, "rootserv:tid")) {
		abend_info.service = mrs[pos++];
		assert(L4_IsGlobalId((L4_ThreadId_t){ .raw = abend_info.service }));
	} else if(streq(name, "uapi:tid")) {
		uapi_info.service = mrs[pos++];
		assert(L4_IsGlobalId((L4_ThreadId_t){ .raw = uapi_info.service }));
	} else {
		printf("%s: name=`%s' unrecognized\n", __func__, name);
	}
}


int main(void)
{
	the_kip = L4_GetKernelInterface();
	add_first_mem();
	start_abend_helper();

	static const struct sysmem_impl_vtable vtab = {
		/* L4.X2 stuff */
		.handle_fault = &impl_handle_fault,

		/* SysInfo stuff */
		.lookup = &impl_lookup,
		.kmsg_block = &impl_kmsg_block,
		.abend_block = &impl_abend_block,
		.uapi_block = &impl_uapi_block,

		/* Sysmem proper */
		.rm_task = &impl_rm_task,
		.send_phys = &impl_send_phys,
		.send_virt = &impl_send_virt,
		.brk = &impl_brk,
		.breath_of_life = &impl_breath_of_life,
		.get_shape = &impl_get_shape,
	};

	for(;;) {
		L4_Word_t status = _muidl_sysmem_impl_dispatch(&vtab);
		if(status == MUIDL_UNKNOWN_LABEL) {
			/* special sysinfo initialization stuff. */
			L4_MsgTag_t tag = muidl_get_tag();
			L4_Word_t mrs[64]; L4_StoreMRs(1, tag.X.u + tag.X.t, mrs);
			L4_ThreadId_t sender = muidl_get_sender();
			if(L4_ThreadNo(sender) < L4_ThreadNo(L4_Myself())
				&& L4_Label(tag) == 0xbaaf)
			{
				sysinfo_init_msg(tag, mrs);
			} else {
				/* do nothing. */
				printf("sysmem: unknown message label=%#lx, u=%lu, t=%lu\n",
					L4_Label(tag), L4_UntypedWords(tag), L4_TypedWords(tag));
			}
		} else if(status != 0 && !MUIDL_IS_L4_ERROR(status)) {
			printf("sysmem: dispatch status %#lx (last tag %#lx)\n",
				status, muidl_get_tag().raw);
		}
	}

	return 0;
}
