
/* systemspace POSIX-like virtual memory server. */

#define VMIMPL_IMPL_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <ccan/minmax/minmax.h>
#include <ccan/array_size/array_size.h>
#include <ccan/likely/likely.h>

#include <l4/types.h>
#include <l4/ipc.h>

#include <ukernel/rangealloc.h>
#include <sneks/mm.h>
#include <sneks/bitops.h>
#include <sneks/rbtree.h>
#include <sneks/process.h>
#include <sneks/hash.h>

#include "nbsl.h"
#include "epoch.h"
#include "defs.h"

#include "muidl.h"
#include "proc-defs.h"
#include "info-defs.h"
#include "fs-defs.h"
#include "vm-impl-defs.h"


struct pl;


/* bitfield accessors. */
#define VP_RIGHTS(vp) ((vp)->vaddr & 7)	/* low 3 = rwx */


/* physical memory. has a short name because it's very common. this structure
 * has the read-only parts of the pp/pl split; the only field that may be
 * concurrently written is ->link, which may be swapped for NULL to take the
 * page off a list (depublication) once the corresponding <struct pl> has been
 * depublished.
 */
struct pp
{
	/* frame number per rangealloc in pp_ra. */
	struct pl *_Atomic link;

	/* pagecache pages only */
	unsigned long fsid;
	uint64_t ino;
	uint32_t offset;	/* limits files to 16 TiB each */
};


/* link to a <struct pp> by physical page number. */
struct pl {
	struct nbsl_node nn;
	uint32_t page_num;	/* constant */
	_Atomic uint32_t status;
};


/* virtual memory page. (there is no <struct vl>.)
 *
 * for now, ->status designates the physical page number used for this vpage.
 * its high bit should always be clear. there will be other formats for
 * swapspace slots, references to pagecache items, copy on write, and so on.
 * this arrangement allows for as many as 2^31 physical pages in vm, or 8 TiB
 * worth.
 */
struct vp {
	uintptr_t vaddr;	/* vaddr in 31..12, flags in 11..0 */
	uint32_t status;	/* complex bitfield, see comment. */
	uint8_t age;
};


/* address space. PID implied. valid when ->kip_area.raw != 0. uninitialized
 * when L4_IsNilFpace(->utcb_area).
 */
struct vm_space
{
	L4_Fpage_t kip_area, utcb_area;
	struct htable pages;	/* vp by int_hash(->vaddr & ~PAGE_MASK) */
	struct rb_root maps;	/* lazy_mmap per range of addr and length */
};


/* set of mmap(2) parameters for lazy pagefault repair. `prot' gets translated
 * to the L4_Rights() mask to test fault access and stored in bits 16..18 of
 * ->flags, i.e. `(flags >> 16) & 7'.
 */
struct lazy_mmap
{
	struct rb_node rb;	/* in vm_space.maps */
	uintptr_t addr;
	size_t length;
	long flags;			/* see main comment wrt rights */
	L4_ThreadId_t fd_serv;
	L4_Word_t fd;
	size_t offset;
};


static size_t pp_first;
static struct rangealloc *pp_ra, *vm_space_ra;

/* all of these contain <struct pl> alone. */
static struct nbsl page_free_list = NBSL_LIST_INIT(page_free_list),
	page_active_list = NBSL_LIST_INIT(page_active_list);

L4_ThreadId_t __uapi_tid;


/* vaddr & ~PAGE_MASK into sp->pages. */
static size_t hash_vp_fn(const void *ptr, void *priv) {
	const struct vp *v = ptr;
	return int_hash(v->vaddr & ~PAGE_MASK);
}

static bool cmp_vp_to_addr(const void *cand, void *key) {
	const struct vp *v = cand;
	L4_Word_t *k = key;
	assert((*k & PAGE_MASK) == 0);
	return (v->vaddr & ~PAGE_MASK) == *k;
}


/* FIXME: this should assert that the pp's link field points to @link. a
 * conditional version can return NULL if it doesn't (i.e. to spot when the
 * physical page's ownership was contested, and the caller lost.)
 */
static inline struct pp *pl2pp(const struct pl *link) {
	return ra_id2ptr(pp_ra, link->page_num - pp_first);
}


/* doesn't discard @oldlink. */
static void push_page(struct nbsl *list, struct pl *oldlink)
{
	struct pl *nl = malloc(sizeof *nl);
	*nl = (struct pl){
		.page_num = oldlink->page_num,
		.status = oldlink->status,
	};
	atomic_store_explicit(&pl2pp(oldlink)->link, nl, memory_order_relaxed);
	struct nbsl_node *top;
	do {
		top = nbsl_top(list);
	} while(!nbsl_push(list, top, &nl->nn));
}


/* dequeues one link from page_free_list and returns it. */
static struct pl *get_free_pl(void)
{
	struct nbsl_node *nod = nbsl_pop(&page_free_list);
	if(nod == NULL) {
		printf("%s: out of memory!\n", __func__);
		abort();
	}

	return container_of(nod, struct pl, nn);
}


static struct lazy_mmap *insert_lazy_mmap_helper(
	struct rb_root *root, struct lazy_mmap *mm)
{
	struct rb_node **p = &root->rb_node, *parent = NULL;
	uintptr_t mm_last = mm->addr + mm->length - 1;
	while(*p != NULL) {
		parent = *p;
		struct lazy_mmap *oth = rb_entry(parent, struct lazy_mmap, rb);
		uintptr_t oth_last = oth->addr + oth->length - 1;
		if(mm_last < oth->addr) p = &(*p)->rb_left;
		else if(mm->addr > oth_last) p = &(*p)->rb_right;
		else return oth;
	}

	__rb_link_node(&mm->rb, parent, p);
	return NULL;
}


static struct lazy_mmap *insert_lazy_mmap(
	struct vm_space *sp, struct lazy_mmap *mm)
{
	struct lazy_mmap *dupe = insert_lazy_mmap_helper(&sp->maps, mm);
	if(dupe == NULL) __rb_insert_color(&mm->rb, &sp->maps);
	return dupe;
}


static struct lazy_mmap *find_lazy_mmap(struct vm_space *sp, uintptr_t addr)
{
	struct rb_node *n = sp->maps.rb_node;
	while(n != NULL) {
		struct lazy_mmap *cand = rb_entry(n, struct lazy_mmap, rb);
		if(addr < cand->addr) n = n->rb_left;
		else if(addr >= cand->addr + cand->length) n = n->rb_right;
		else return cand;
	}
	return NULL;
}


static COLD void init_phys(L4_Fpage_t *phys, int n_phys)
{
	size_t p_min = ~0ul, p_max = 0;
	for(int i=0; i < n_phys; i++) {
		p_min = min_t(size_t, p_min, L4_Address(phys[i]) >> PAGE_BITS);
		p_max = max_t(size_t, p_max,
			(L4_Address(phys[i]) + L4_Size(phys[i])) >> PAGE_BITS);
	}
	size_t p_total = p_max - p_min;
	printf("vm: allocating %lu <struct pp> (first is %lu)\n",
		(unsigned long)p_total, (unsigned long)p_min);
	pp_first = p_min;

	int eck = e_begin();
	pp_ra = RA_NEW(struct pp, p_total);
	for(int i=0; i < n_phys; i++) {
		int base = (L4_Address(phys[i]) >> PAGE_BITS) - pp_first;
		assert(base >= 0);
		for(int o=0; o < L4_Size(phys[i]) >> PAGE_BITS; o++) {
			struct pp *pp = ra_alloc(pp_ra, base + o);
			assert(ra_ptr2id(pp_ra, pp) == base + o);
			assert(ra_id2ptr(pp_ra, base + o) == pp);
			struct pl *link = malloc(sizeof *link);
			link->page_num = base + o;
			atomic_store_explicit(&link->status, 0, memory_order_relaxed);
			atomic_store(&pp->link, link);
			struct nbsl_node *top;
			do {
				top = nbsl_top(&page_free_list);
			} while(!nbsl_push(&page_free_list, top, &link->nn));
		}
	}
	e_end(eck);
}


static inline int prot_to_l4_rights(int prot)
{
	int m = (prot & PROT_READ) << 2
		| (prot & PROT_WRITE)
		| (prot & PROT_EXEC) >> 2;
	assert(!!(m & L4_Readable) == !!(prot & PROT_READ));
	assert(!!(m & L4_Writable) == !!(prot & PROT_WRITE));
	assert(!!(m & L4_eXecutable) == !!(prot & PROT_EXEC));
	return m;
}


static int vm_mmap(
	uint16_t target_pid, L4_Word_t *addr_ptr, uint32_t length,
	int32_t prot, int32_t flags,
	L4_Word_t fd_serv, L4_Word_t fd, uint32_t offset)
{
#if 0
	printf("%s: target_pid=%u, addr=%#lx, length=%#x, offset=%u\n",
		__func__, target_pid, *addr_ptr, length, offset);
#endif

	int sender_pid = pidof_NP(muidl_get_sender());
	if(target_pid == 0) target_pid = sender_pid;
	if(target_pid > SNEKS_MAX_PID || target_pid == 0) return -EINVAL;
	if((*addr_ptr & PAGE_MASK) != 0) return -EINVAL;

	struct vm_space *sp = ra_id2ptr(vm_space_ra, target_pid);
	if(sp->kip_area.raw == 0) return -EINVAL;

	if(*addr_ptr != 0) {
		/* TODO: cut addr, length up by kip_area, utcb_area, sbrk, and all the
		 * existing maps.
		 */
	} else {
		/* FIXME: assign *addr_ptr a good ways forward of the sbrk heap,
		 * around existing reservations.
		 */
		printf("%s: allocating mmap not implemented\n", __func__);
		return -ENOSYS;
	}
	struct lazy_mmap *mm = malloc(sizeof *mm);
	if(mm == NULL) return -ENOMEM;
	*mm = (struct lazy_mmap){
		.addr = *addr_ptr,
		.length = (length + PAGE_SIZE - 1) & ~PAGE_MASK,
		.flags = flags | (prot_to_l4_rights(prot) << 16),
		.fd_serv.raw = fd_serv, .fd = fd, .offset = offset,
	};
	struct lazy_mmap *old = insert_lazy_mmap(sp, mm);
	if(old != NULL) {
		/* TODO: turn this check into an assert once addr, length is properly
		 * cut up
		 */
		free(mm);
		return -EEXIST;
	}

	return 0;
}


static int vm_fork(uint16_t srcpid, uint16_t destpid)
{
	if(srcpid > SNEKS_MAX_PID || destpid > SNEKS_MAX_PID) return -EINVAL;

	int sender_pid = pidof_NP(muidl_get_sender());
	if(sender_pid < SNEKS_MIN_SYSID) return -EINVAL;

	struct vm_space *src;
	if(srcpid == 0) {
		/* spawn case. */
		src = NULL;
	} else if(srcpid <= SNEKS_MAX_PID) {
		/* user fork case. */
		src = ra_id2ptr(vm_space_ra, srcpid);
		if(src->kip_area.raw == 0) return -EINVAL;
	} else {
		/* values in between. */
		return -EINVAL;
	}

	struct vm_space *dest = ra_alloc(vm_space_ra,
		destpid > 0 ? destpid : -1);
	if(dest == NULL) return -EEXIST;
	dest->kip_area.raw = ~0ul;
	dest->utcb_area = L4_Nilpage;
	htable_init(&dest->pages, &hash_vp_fn, NULL);
	dest->maps = RB_ROOT;

	if(src != NULL) {
		/* FIXME: actually fork @src's stuff over. */
		htable_clear(&dest->pages);
		ra_free(vm_space_ra, dest);
		return -ENOSYS;
	}

	return ra_ptr2id(vm_space_ra, dest);
}


static int vm_set_kernel_areas(uint16_t pid, L4_Fpage_t utcb, L4_Fpage_t kip)
{
	if(pid > SNEKS_MAX_PID) return -EINVAL;
	struct vm_space *sp = ra_id2ptr(vm_space_ra, pid);
	if(sp->kip_area.raw == 0) return -EINVAL;
	if(sp->kip_area.raw != ~0ul || !L4_IsNilFpage(sp->utcb_area)) {
		/* TODO: should be "illegal state" */
		return -EINVAL;
	}

	/* FIXME: check for kip, utcb overlap with maps, pop errors if they do. */

	sp->kip_area = kip;
	sp->utcb_area = utcb;

	return 0;
}


static int vm_upload_page(
	uint16_t target_pid, L4_Word_t addr,
	const uint8_t *data, unsigned data_len)
{
#if 0
	printf("%s: target_pid=%u, addr=%#lx, data_len=%u\n", __func__,
		target_pid, addr, data_len);
#endif

	struct vm_space *sp = ra_id2ptr(vm_space_ra, target_pid);
	if(unlikely(L4_IsNilFpage(sp->utcb_area))) return -EINVAL;
	if((addr & PAGE_MASK) != 0) return -EINVAL;

	struct vp *v = malloc(sizeof *v);
	if(unlikely(v == NULL)) return -ENOMEM;
	v->vaddr = addr | L4_FullyAccessible;
	v->age = 1;

	size_t hash = int_hash(addr);
	struct vp *old = htable_get(&sp->pages, hash, &cmp_vp_to_addr, &addr);
	if(old != NULL) {
		htable_del(&sp->pages, hash, old);
		/* FIXME: drop the physical page as well */
		free(old);
	}

	/* allocate fresh new anonymous memory for this. */
	int eck = e_begin();
	struct pl *link = get_free_pl();
	uint8_t *mem = (uint8_t *)(link->page_num << PAGE_BITS);
	memcpy(mem, data, data_len);
	if(data_len < PAGE_SIZE) {
		memset(mem + data_len, '\0', PAGE_SIZE - data_len);
	}
	push_page(&page_active_list, link);
	e_free(link);
	v->status = (L4_Word_t)mem >> PAGE_BITS;
	e_end(eck);

	/* plunk it in there. */
	assert(hash_vp_fn(v, NULL) == hash);
	bool ok = htable_add(&sp->pages, hash, v);
	if(unlikely(!ok)) {
		/* FIXME: roll back and return an error. */
		printf("%s: can't handle failing htable_add()!\n", __func__);
		abort();
	}

	return 0;
}


static void vm_breath_of_life(
	L4_Word_t *rc_p,
	L4_Word_t target_raw, L4_Word_t sp, L4_Word_t ip)
{
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 2 }.raw);
	L4_LoadMR(1, ip);
	L4_LoadMR(2, sp);
	L4_MsgTag_t tag = L4_Reply((L4_ThreadId_t){ .raw = target_raw });
	*rc_p = L4_IpcFailed(tag) ? L4_ErrorCode() : 0;
}


static void vm_iopf(L4_Fpage_t fault, L4_Word_t fip, L4_MapItem_t *page_ptr)
{
	/* userspace tasks can't have I/O ports. at all. it is verboten. */
	printf("%s: IO fault from pid=%d, ip=%#lx, port=%#lx:%#lx\n",
		__func__, pidof_NP(muidl_get_sender()), fip,
		L4_IoFpagePort(fault), L4_IoFpageSize(fault));
	muidl_raise_no_reply();
}


static void vm_pf(L4_Word_t faddr, L4_Word_t fip, L4_MapItem_t *map_out)
{
	int n, pid = pidof_NP(muidl_get_sender());
	if(unlikely(pid > SNEKS_MAX_PID)) {
		printf("%s: fault from pid=%d (tid=%lu:%lu)?\n", __func__, pid,
			L4_ThreadNo(muidl_get_sender()), L4_Version(muidl_get_sender()));
		muidl_raise_no_reply();
		return;
	}
	struct vm_space *sp = ra_id2ptr(vm_space_ra, pid);
	if(unlikely(L4_IsNilFpage(sp->utcb_area))) {
		printf("%s: faulted into uninitialized space (pid=%d)\n",
			__func__, pid);
		muidl_raise_no_reply();
		return;
	}

	//printf("%s: pid=%d, faddr=%#lx, fip=%#lx\n", __func__, pid, faddr, fip);

	int eck = e_begin();

	const int fault_rwx = L4_Label(muidl_get_tag()) & 7;
	L4_Word_t faddr_page = faddr & ~PAGE_MASK;
	size_t hash = int_hash(faddr_page);
	struct vp *old = htable_get(&sp->pages, hash,
		&cmp_vp_to_addr, &faddr_page);
	if(old != NULL && (VP_RIGHTS(old) & fault_rwx) != fault_rwx) {
		printf("%s: segv (access mode %#x, old page had %#x)\n", __func__,
			fault_rwx, VP_RIGHTS(old));
		goto segv;
	} else if(old != NULL) {
		/* quick remap or expand. */
#if 0
		printf("%s: remap of %#lx\n", __func__,
			(L4_Word_t)old->status << PAGE_BITS);
#endif
		L4_Fpage_t map_page = L4_FpageLog2(
			old->status << PAGE_BITS, PAGE_BITS);
		L4_Set_Rights(&map_page, VP_RIGHTS(old) & fault_rwx);
		*map_out = L4_MapItem(map_page, faddr & ~PAGE_MASK);
		e_end(eck);
		return;
	}

	/* TODO: check program break (quickly) */

	struct lazy_mmap *mm = find_lazy_mmap(sp, faddr);
	if(unlikely(mm == NULL)) {
		printf("%s: segv (unmapped)\n", __func__);
		goto segv;
	}
	assert(faddr >= mm->addr && faddr < mm->addr + mm->length);
	int mm_rwx = (mm->flags >> 16) & 7;
	if(unlikely((fault_rwx & mm_rwx) != fault_rwx)) {
		printf("%s: segv (access mode, mmap)\n", __func__);
		goto segv;
	}

	uint8_t *page;
	if((mm->flags & MAP_ANONYMOUS) != 0) {
		struct pl *link = get_free_pl();
		page = (uint8_t *)((uintptr_t)link->page_num << PAGE_BITS);
		memset(page, '\0', PAGE_SIZE);
		push_page(&page_active_list, link);
		e_free(link);
		//printf("%s: anon memory! new page is %p\n", __func__, page);
	} else {
		struct pl *link = get_free_pl();
		page = (uint8_t *)((uintptr_t)link->page_num << PAGE_BITS);
		unsigned n_bytes = PAGE_SIZE;
		n = __fs_read(mm->fd_serv, page, &n_bytes, mm->fd,
			mm->offset + ((faddr & ~PAGE_MASK) - mm->addr), PAGE_SIZE);
		if(unlikely(n != 0)) {
			push_page(&page_free_list, link);
			e_free(link);
			printf("%s: I/O error on file mapping: n=%d\n", __func__, n);
			goto segv;
		}
		if(n_bytes < PAGE_SIZE) {
			printf("%s: short read (got %u bytes)\n", __func__, n_bytes);
			memset(&page[n_bytes], '\0', PAGE_SIZE - n_bytes);
		}
		push_page(&page_active_list, link);
		e_free(link);
		//printf("%s: file memory! new page is %p\n", __func__, page);
#if 0
		for(int i=0; i < 64; i++) {
			if(i > 0 && (i & 15) == 0) printf("\n");
			printf("%02x ", page[i]);
		}
		printf("\n");
#endif
	}

	struct vp *vp = malloc(sizeof *vp);
	if(unlikely(vp == NULL)) {
		/* FIXME */
		abort();
	}
	vp->status = faddr_page | mm_rwx;
	vp->age = 1;
	vp->status = (L4_Word_t)page >> PAGE_BITS;
	bool ok = htable_add(&sp->pages, hash_vp_fn(vp, NULL), vp);
	if(unlikely(!ok)) {
		/* FIXME: roll back and segfault. */
		printf("%s: can't handle failing htable_add()!\n", __func__);
		abort();
	}

	L4_Fpage_t map_page = L4_FpageLog2((L4_Word_t)page, PAGE_BITS);
	L4_Set_Rights(&map_page, (mm->flags >> 16) & 7);
	*map_out = L4_MapItem(map_page, faddr & ~PAGE_MASK);

	e_end(eck);
	return;

segv:
	/* pop segfault and don't reply. */
	n = __proc_kill(__uapi_tid, pid, SIGSEGV);
	if(n != 0) {
		printf("%s: Proc::kill() failed, n=%d\n", __func__, n);
	}
	muidl_raise_no_reply();
	e_end(eck);
}


static COLD void get_services(void)
{
	struct sneks_uapi_info u;
	int n = __info_uapi_block(L4_Pager(), &u);
	if(n != 0) {
		printf("%s: can't get UAPI block, n=%d\n", __func__, n);
		abort();
	}
	__uapi_tid.raw = u.service;
}


int main(int argc, char *argv[])
{
	printf("vm sez hello!\n");
	L4_ThreadId_t init_tid;
	int n_phys = 0;
	L4_Fpage_t *phys = init_protocol(&n_phys, &init_tid);
	printf("vm: init protocol done.\n");

	init_phys(phys, n_phys);
	free(phys);
	printf("vm: physical memory tracking initialized.\n");

	/* the rest of the owl */
	vm_space_ra = RA_NEW(struct vm_space, SNEKS_MAX_PID + 1);
	get_services();

	L4_LoadMR(0, 0);
	L4_MsgTag_t tag = L4_Reply(init_tid);
	if(L4_IpcFailed(tag)) {
		printf("vm: reply to init_tid=%lu:%lu failed, ec=%lu\n",
			L4_ThreadNo(init_tid), L4_Version(init_tid), L4_ErrorCode());
		abort();
	}

	/* main IPC loop. */
	static const struct vm_impl_vtable vtab = {
		/* Sneks::VM */
		.mmap = &vm_mmap,
		.fork = &vm_fork,
		.set_kernel_areas = &vm_set_kernel_areas,
		.upload_page = &vm_upload_page,
		.breath_of_life = &vm_breath_of_life,

		/* L4X2::FaultHandler */
		.handle_fault = &vm_pf,
		/* L4X2::X86IOFaultHandler */
		.handle_x86_io_fault = &vm_iopf,
	};
	for(;;) {
		L4_Word_t status = _muidl_vm_impl_dispatch(&vtab);
		if(status != 0 && !MUIDL_IS_L4_ERROR(status)) {
			printf("vm: dispatch status %#lx (last tag %#lx)\n",
				status, muidl_get_tag().raw);
		}
	}

	return 0;
}
