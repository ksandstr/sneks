
#define SYSMEM_IMPL_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdalign.h>
#include <string.h>
#include <errno.h>
#include <ccan/list/list.h>
#include <ccan/likely/likely.h>
#include <ccan/minmax/minmax.h>

#include <sneks/rbtree.h>
#include <sneks/mm.h>

#include <l4/types.h>
#include <l4/kip.h>
#include <l4/ipc.h>
#include <l4/space.h>
#include <l4/thread.h>
#include <l4/kdebug.h>

#include "muidl.h"
#include "sysmem-defs.h"


#define NUM_INIT_PAGES 12	/* traditionally enough. */


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
};


/* logical memory.
 * TODO: this has a hideous per-page overhead but it'll do for now. the
 * rb-tree is used to find the associated systask so it's not all bad.
 */
struct l_page
{
	struct rb_node rb;	/* in systask.mem */
	L4_Word_t l_addr;	/* address in task */
	/* bits 12..31 indicate syspager address of the physical page when nonzero.
	 * bits 0..11 are a mask of LF_*, flags of the logical memory.
	 */
	L4_Word_t p_addr;
};


/* a system task tracked by sysmem. */
struct systask {
	struct rb_root mem;			/* of <struct l_page> via rb */
	struct list_head threads;	/* of <struct st_thr> */
	uintptr_t brk;	/* program break (inclusive) */
};


struct st_thr {
	struct rb_node rb;			/* in all_threads, per L4_ThreadNo(tid) */
	struct list_node link;		/* in systask.threads */
	struct systask *task;
	L4_ThreadId_t tid;
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


/* TODO: move these into an integer utility header */
#define MSB(x) (sizeof((x)) * 8 - __builtin_clzl((x)) - 1)

static inline int size_to_shift(size_t sz) {
	int msb = MSB(sz);
	return (1 << msb) < sz ? msb + 1 : msb;
}


/* note that this leaves .pages NULL, requiring initialization in alloc(). */
#define ALLOC_HEAD(typ) ((struct alloc_head){ \
	.sz = sizeof(typ), .align = alignof(typ), \
	.trash = NULL })


L4_KernelInterfacePage_t *the_kip;

static struct list_head free_page_list = LIST_HEAD_INIT(free_page_list),
	active_page_list = LIST_HEAD_INIT(active_page_list),
	reserved_page_list = LIST_HEAD_INIT(reserved_page_list);

static struct alloc_head systask_alloc_head = ALLOC_HEAD(struct systask),
	st_thr_alloc_head = ALLOC_HEAD(struct st_thr),
	p_page_alloc_head = ALLOC_HEAD(struct p_page),
	l_page_alloc_head = ALLOC_HEAD(struct l_page);

static struct rb_root all_threads = RB_ROOT;

static uint8_t first_mem[PAGE_SIZE * NUM_INIT_PAGES]
	__attribute__((aligned(PAGE_SIZE)));


/* runtime basics. */

NORETURN void panic(const char *msg)
{
	/* TODO: trigger system panic */
	printf("sysmem: PANIC (%s)\n", msg);
	for(;;) { L4_Sleep(L4_Never); }
}


void abort(void) {
	panic("sysmem aborted");
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

static inline int thr_cmp(const struct st_thr *a, const struct st_thr *b) {
	return (int)L4_ThreadNo(a->tid) - (int)L4_ThreadNo(b->tid);
}


static struct st_thr *get_thr(L4_ThreadId_t tid)
{
	assert(L4_IsGlobalId(tid));
	struct rb_node *n = all_threads.rb_node;
	int a = L4_ThreadNo(tid);
	while(n != NULL) {
		struct st_thr *t = rb_entry(n, struct st_thr, rb);
		int b = L4_ThreadNo(t->tid);
		if(a < b) n = n->rb_left;
		else if(a > b) n = n->rb_right;
		else return t;
	}
	return NULL;
}


static inline struct st_thr *put_thr_helper(struct st_thr *t)
{
	struct rb_node **p = &all_threads.rb_node, *parent = NULL;
	int a = L4_ThreadNo(t->tid);
	while(*p != NULL) {
		parent = *p;
		struct st_thr *oth = rb_entry(parent, struct st_thr, rb);
		if(a < L4_ThreadNo(oth->tid)) p = &(*p)->rb_left;
		else if(a > L4_ThreadNo(oth->tid)) p = &(*p)->rb_right;
		else return oth;
	}

	rb_link_node(&t->rb, parent, p);
	return NULL;
}


static void put_thr(struct st_thr *t)
{
	struct st_thr *dupe = put_thr_helper(t);
	if(dupe != NULL) panic("duplicate in put_thr()!");
	rb_insert_color(&t->rb, &all_threads);

	assert(get_thr(t->tid) == t);
}


static struct systask *get_task(L4_ThreadId_t tid) {
	struct st_thr *t = get_thr(tid);
	return t != NULL ? t->task : NULL;
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


void free(void *ptr) {
	printf("sysmem: tried to free(ptr=%p); does nothing\n", ptr);
}


/* sub-page memory allocation and recycling. */

#define alloc_struct(name) (struct name *)alloc(&name ## _alloc_head)
#define recycle_struct(typ, ptr) recycle(&name ## _alloc_head, (ptr))

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
		/* get more ram. FIXME: use sophisticated methods. */
		phys = list_pop(&free_page_list, struct p_page, link);
		if(phys == NULL) panic("can't get more ram!");
		assert(phys->owner == NULL);
		list_add(&head->pages, &phys->link);
		p = (struct alloc_page *)phys->address;
		p->next = (void *)phys->address
			+ ((sizeof *p + head->align - 1) & ~(head->align - 1));
	}
	void *ptr = p->next;
	p->next += (head->sz + head->align - 1) & ~(head->align - 1);
	if(p->next >= (void *)phys->address + PAGE_SIZE) p->next = NULL;

	return ptr;
}


/* (can't call it free().) */
static void recycle(struct alloc_head *head, void *ptr)
{
	memset(ptr + sizeof(void *), 0xde, head->sz - sizeof(void *));
	*(void **)ptr = head->trash;
	head->trash = ptr;
}


/* implementation of sneks::Sysmem */

static void impl_handle_fault(
	L4_Word_t faddr, L4_Word_t fip, L4_MapItem_t *page_ptr)
{
	L4_ThreadId_t tid = muidl_get_sender();
#if 0
	printf("pf in %lu:%lu, faddr=%#lx, fip=%#lx\n",
		L4_ThreadNo(tid), L4_Version(tid), faddr, fip);
#endif

	if((faddr & ~PAGE_MASK) == 0) {
		printf("sysmem: null page access\n");
		goto failed;
	}

	struct systask *task = get_task(tid);
	if(task == NULL) {
		printf("sysmem: unknown task\n");
		goto failed;
	}

	struct l_page *lp = get_lpage(task, faddr & ~PAGE_MASK);
	if(lp == NULL && (faddr & ~PAGE_MASK) > task->brk) {
		printf("system task %lu:%lu segfaulted at faddr=%#lx, fip=%#lx\n",
			L4_ThreadNo(tid), L4_Version(tid), faddr, fip);
		goto failed;
	} else if(lp == NULL) {
		lp = alloc_struct(l_page);
		struct p_page *phys = list_pop(&free_page_list, struct p_page, link);
		if(phys == NULL) panic("can't get more ram!");
		lp->p_addr = phys->address;
		lp->l_addr = faddr & ~PAGE_MASK;
		phys->owner = lp;
		put_lpage(task, lp);
		list_add_tail(&active_page_list, &phys->link);
	}

	L4_Fpage_t fp = L4_FpageLog2(lp->p_addr & ~PAGE_MASK, PAGE_BITS);
	L4_Set_Rights(&fp, L4_FullyAccessible);
	*page_ptr = L4_MapItem(fp, faddr & ~PAGE_MASK);
	return;

failed:
	muidl_raise_no_reply();
}


static void impl_new_task(
	L4_Fpage_t kip_area, L4_Fpage_t utcb_area, L4_Word_t first_thread_raw)
{
	L4_ThreadId_t first_thread = { .raw = first_thread_raw };
	if(L4_IsLocalId(first_thread) || L4_IsNilThread(first_thread)) {
		printf("sysmem: invalid first_thread=%#lx\n", first_thread.raw);
		goto fail;
	} else if(get_thr(first_thread) != NULL) {
		printf("sysmem: tried to duplicate first_thread=%lu:%lu\n",
			L4_ThreadNo(first_thread), L4_Version(first_thread));
		goto fail;
	}

	struct systask *task = alloc_struct(systask);
	struct st_thr *t = alloc_struct(st_thr);
	task->mem = RB_ROOT;
	task->brk = 0x10000;	/* 64k should be enough for everyone */
	list_head_init(&task->threads);
	t->tid = first_thread;
	t->task = task;
	list_add_tail(&task->threads, &t->link);
	put_thr(t);

	return;

fail:
	panic("sysmem is very confused and sad");
}


static void impl_add_thread(L4_Word_t space, L4_Word_t new_thread)
{
	L4_ThreadId_t tid = { .raw = new_thread };
	if(get_thr(tid) != NULL) {
		printf("sysmem: new_thread=%lu:%lu exists\n",
			L4_ThreadNo(tid), L4_Version(tid));
		panic("add_thread EEXIST");
	}

	struct systask *task = get_task((L4_ThreadId_t){ .raw = space });
	if(task == NULL) {
		printf("sysmem: space=%#lx doesn't exist\n", space);
		panic("add_thread ENOENT");
	}
	struct st_thr *t = alloc_struct(st_thr);
	t->tid = tid;
	t->task = task;
	list_add_tail(&task->threads, &t->link);
	put_thr(t);
}


static void impl_rm_thread(L4_Word_t space, L4_Word_t gone_thread)
{
	panic("rm_thread not implemented");
}


static void impl_send_phys(
	L4_Word_t dest_raw, L4_Word_t frame_num, int size_log2)
{
	L4_ThreadId_t dest_tid = { .raw = dest_raw };
	assert(L4_IsNilThread(dest_tid) || L4_IsGlobalId(dest_tid));
	struct systask *task = NULL;
	if(!L4_IsNilThread(dest_tid)) {
		task = get_task(dest_tid);
		if(task == NULL) return;
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

	L4_Accept(L4_MapGrantItems(L4_FpageLog2(phys_addr, size_log2)));
	L4_MsgTag_t tag = L4_Receive_Timeout(muidl_get_sender(),
		L4_TimePeriod(5 * 1000));
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
		printf("out-of-band message in send_phys!\n");
		goto abnormal;
	}

	if(task == NULL) {
		/* "any old RAM" mode */
		struct list_head *list;
		/* recognize sysmem's own pages, add them to the reserved list. */
		extern char _start, _end;
		if(phys_addr >= (L4_Word_t)&_start && phys_addr < (L4_Word_t)&_end) {
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

		if(list != NULL) list_add_tail(list, &pg->link);
	} else {
		/* RAM for things loaded as boot modules. */
		struct l_page *lp = alloc_struct(l_page);
		lp->l_addr = phys_addr;
		lp->p_addr = phys_addr;
		put_lpage(task, lp);
		pg->owner = lp;
		list_add_tail(&active_page_list, &pg->link);
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
	L4_ThreadId_t src_tid = muidl_get_sender(),
		dest_tid = { .raw = dest_tid_raw };
	struct systask *src = get_task(src_tid),
		*dest = get_task(dest_tid);
	if(src == NULL || dest == NULL) return ENOENT;

	struct l_page *lp = get_lpage(src, src_addr);
	if(lp == NULL) return EFAULT;

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

	return 0;
}


static void impl_brk(L4_Word_t new_brk)
{
	L4_ThreadId_t sender = muidl_get_sender();
	struct systask *task = get_task(sender);
	if(task == NULL) {
		printf("sysmem: %s: no such task\n", __func__);
		return;
	}

	/* TODO: handle new_brk < task->brk by marking logical pages
	 * instareplaceable, or throwing their compressed contents away where
	 * relevant.
	 */
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


static void add_first_mem(void)
{
	for(int i=0; i < NUM_INIT_PAGES; i++) {
		struct p_page *phys = malloc(sizeof *phys);
		phys->address = (L4_Word_t)&first_mem[i * PAGE_SIZE];
		phys->owner = NULL;
		list_add_tail(&free_page_list, &phys->link);
	}
}


int main(void)
{
	the_kip = L4_GetKernelInterface();
	add_first_mem();

	static const struct sysmem_vtable vtab = {
		.handle_fault = &impl_handle_fault,
		.new_task = &impl_new_task,
		.add_thread = &impl_add_thread,
		.rm_thread = &impl_rm_thread,
		.send_phys = &impl_send_phys,
		.send_virt = &impl_send_virt,
		.brk = &impl_brk,
		.breath_of_life = &impl_breath_of_life,
	};

	for(;;) {
		L4_Word_t status = _muidl_sysmem_dispatch(&vtab);
		if(status == MUIDL_UNKNOWN_LABEL) {
			/* do nothing. */
		} else if(status != 0 && !MUIDL_IS_L4_ERROR(status)) {
			printf("sysmem: dispatch status %#lx (last tag %#lx)\n",
				status, muidl_get_tag().raw);
		}
	}

	return 0;
}
