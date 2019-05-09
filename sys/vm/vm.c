
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
#include <ccan/container_of/container_of.h>

#include <l4/types.h>
#include <l4/ipc.h>
#include <l4/space.h>

#include <ukernel/rangealloc.h>
#include <sneks/mm.h>
#include <sneks/bitops.h>
#include <sneks/rbtree.h>
#include <sneks/process.h>
#include <sneks/hash.h>
#include <sneks/sysinfo.h>

#include "nbsl.h"
#include "epoch.h"
#include "defs.h"

#include "muidl.h"
#include "proc-defs.h"
#include "info-defs.h"
#include "fs-defs.h"
#include "vm-impl-defs.h"


#ifndef TRACE_FAULTS
#define TRACE_FAULTS 0
#endif


struct pl;


/* flags etc. in vp->vaddr's low 12 bits */
#define VPM_RIGHTS 0x7
#define VPF_COW 0x8


/* bitfield accessors. */
#define VP_RIGHTS(vp) ((vp)->vaddr & 7)	/* low 3 = rwx */
#define VP_IS_COW(vp) (((vp)->vaddr & VPF_COW) != 0)


struct vp;

/* physical memory. has a short name because it's very common. this structure
 * has the read-only parts of the pp/pl split; the only field that may be
 * concurrently written is ->link, which may be swapped for NULL to take the
 * page off a list (depublication) once the corresponding <struct pl> has been
 * depublished.
 */
struct pp
{
	/* frame number implied via pp_ra. */
	struct pl *_Atomic link;
	struct vp *_Atomic owner;	/* primary owner. see share_table. */

	/* for pagecache pages only.
	 * (TODO: move into <struct pl> since these are set when list membership
	 * changes.)
	 */
	unsigned long fsid;
	uint64_t ino;
	uint32_t offset;	/* limits files to 16 TiB each */
};


/* link to a <struct pp> by physical page number. */
struct pl {
	struct nbsl_node nn;
	uint32_t page_num;
	_Atomic uint32_t status;
};


/* virtual memory page. (there is no <struct vl>.)
 *
 * for now, ->status designates the physical page number used for this vpage.
 * its high bit should always be clear. there will be other formats for
 * swapspace slots, references to pagecache items, copy on write, and so on.
 * this arrangement allows for as many as 2^31 physical pages in vm, or 8 TiB
 * worth. if ->status is 0, anonymous memory has not yet been attached to this
 * page by the fault handler.
 *
 * flags are assigned in ->vaddr's low 12 bits as follows:
 *   - 2..0 are a mask of L4_Readable, L4_Writable, and L4_eXecutable,
 *     designating the access granted to the associated address space.
 *   - bits 11..3 are not used and should be left clear.
 */
struct vp {
	uintptr_t vaddr;	/* vaddr in 31..12, flags in 11..0 */
	uint32_t status;	/* complex format, see comment. */
	uint8_t age;		/* TODO: move into subfield of pl->status */
};


/* address space. PID implied. valid when ->kip_area.raw != 0. uninitialized
 * when L4_IsNilFpage(->utcb_area).
 */
struct vm_space
{
	L4_Fpage_t kip_area, utcb_area, sysinfo_area;
	struct htable pages;	/* in vm_space.pages with hash_vp_by_vaddr() */
	struct rb_root maps;	/* lazy_mmap per range of addr and length */
	L4_Word_t brk;			/* top of non-mmap heap */
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


static size_t hash_vp_by_phys(const void *ptr, void *priv);

static size_t pp_first;
static struct rangealloc *pp_ra, *vm_space_ra;

/* all of these contain <struct pl> alone. */
static struct nbsl page_free_list = NBSL_LIST_INIT(page_free_list),
	page_active_list = NBSL_LIST_INIT(page_active_list);

static struct htable share_table = HTABLE_INITIALIZER(
	share_table, &hash_vp_by_phys, NULL);

L4_ThreadId_t __uapi_tid;


/* vaddr & ~PAGE_MASK into sp->pages. */
static size_t hash_vp_by_vaddr(const void *ptr, void *priv) {
	const struct vp *v = ptr;
	return int_hash(v->vaddr & ~PAGE_MASK);
}

static bool cmp_vp_to_vaddr(const void *cand, void *key) {
	const struct vp *v = cand;
	L4_Word_t *k = key;
	assert((*k & PAGE_MASK) == 0);
	return (v->vaddr & ~PAGE_MASK) == *k;
}


/* by physical page number in share_table. */
static size_t hash_vp_by_phys(const void *ptr, void *priv) {
	const struct vp *v = ptr;
	assert(v->status != 0);
	/* TODO: assert that high bit is clear in v->status, i.e. that we're
	 * hashing a physical frame number here.
	 */
	return int_hash(v->status);
}

static bool cmp_vp_to_phys(const void *cand, void *key) {
	const struct vp *v = cand;
	assert(v->status != 0);
	/* TODO: see above, same thing here */
	uint32_t *page_num = key;
	return v->status == *page_num;
}


/* FIXME: this should assert that the pp's link field points to @link. a
 * conditional version can return NULL if it doesn't (i.e. to spot when the
 * physical page's ownership was contested, and the caller lost.)
 */
static inline struct pp *pl2pp(const struct pl *link) {
	return ra_id2ptr(pp_ra, link->page_num - pp_first);
}


static struct pp *get_pp(uint32_t page_num) {
	assert(page_num >= pp_first);
	return ra_id2ptr(pp_ra, page_num - pp_first);
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

	if(length == 0) return -EINVAL;
	if((*addr_ptr | offset) & PAGE_MASK) return -EINVAL;
	if((flags & MAP_PRIVATE) && (flags & MAP_SHARED)) return -EINVAL;
	if((~flags & MAP_PRIVATE) && (~flags & MAP_SHARED)) return -EINVAL;

	int sender_pid = pidof_NP(muidl_get_sender());
	if(target_pid == 0) target_pid = sender_pid;
	if(target_pid > SNEKS_MAX_PID || target_pid == 0) return -EINVAL;

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


static int vm_brk(L4_Word_t addr)
{
	int sender_pid = pidof_NP(muidl_get_sender());
	if(unlikely(sender_pid >= SNEKS_MAX_PID)) return -EINVAL;

	printf("%s: sender_pid=%d, addr=%#lx\n", __func__, sender_pid, addr);
	struct vm_space *sp = ra_id2ptr(vm_space_ra, sender_pid);
	assert(!L4_IsNilFpage(sp->utcb_area));

	/* FIXME: pop an error if @addr is past the end of userspace virtual
	 * memory range.
	 */

	if(addr < sp->brk) {
		/* TODO: find and release pages that aren't associated with
		 * a lazy_mmap.
		 */
	} else if(addr > sp->brk && sp->brk != 0) {
		/* allocate virtual memory for the new range. */
		assert((sp->brk & PAGE_MASK) == 0);
		for(L4_Word_t a = sp->brk; a < addr; a += PAGE_SIZE) {
			size_t hash = int_hash(a);
			struct vp *v = htable_get(&sp->pages, hash, &cmp_vp_to_vaddr, &a);
			if(v != NULL) continue;
			v = malloc(sizeof *v);
			if(unlikely(v == NULL)) {
				/* FIXME: roll back and error out */
				abort();
			}
			*v = (struct vp){ .vaddr = a | L4_FullyAccessible, .age = 1 };
			assert(hash_vp_by_vaddr(v, NULL) == hash);
			bool ok = htable_add(&sp->pages, hash, v);
			if(unlikely(!ok)) {
				/* FIXME. see above */
				abort();
			}
		}
	}

	sp->brk = addr & ~PAGE_MASK;	/* end of data segment (exclusive) */

	return 0;
}


/* copy lazy_mmaps. */
static int fork_maps(struct vm_space *src, struct vm_space *dest)
{
	for(struct rb_node *cur = __rb_first(&src->maps);
		cur != NULL;
		cur = __rb_next(cur))
	{
		struct lazy_mmap *orig = container_of(cur, struct lazy_mmap, rb),
			*copy = malloc(sizeof *copy);
		if(copy == NULL) {
			/* FIXME: cleanup! */
			return -ENOMEM;
		}
		*copy = *orig;
		/* FIXME: duplicate file descriptors etc. */
		void *dupe = insert_lazy_mmap(dest, copy);
		assert(dupe == NULL);
	}
	return 0;
}


static int fork_pages(struct vm_space *src, struct vm_space *dest)
{
	/* NOTE: we could malloc src->pages.elems instances of <struct vp> to
	 * avoid checking malloc return values mid-loop, and dodge some rollback
	 * overhead in case one of them doesn't work out. dlmalloc has a fancy
	 * dlindependent_calloc() function for doing just that, and likely quicker
	 * than a sequence of scattered mallocs. (however, this is optimizations,
	 * and there's no benchmark.)
	 */
	L4_Fpage_t unmaps[64];
	int unmap_pos = 0, n_pages = 0;
	struct htable_iter it;
	for(struct vp *cur = htable_first(&src->pages, &it);
		cur != NULL;
		cur = htable_next(&src->pages, &it))
	{
		n_pages++;
		if(cur->status == 0) {
			/* replicate unmapped brk area. */
			assert(cur->vaddr < src->brk);
			struct vp *copy = malloc(sizeof *copy);
			if(copy == NULL) goto Enomem;
			*copy = (struct vp){ .vaddr = cur->vaddr, .age = 1 };
			bool ok = htable_add(&dest->pages,
				hash_vp_by_vaddr(copy, NULL), copy);
			if(!ok) goto Enomem;
			continue;
		}

		/* currently we only need handle private anonymous memory, which is
		 * always COW'd when writable, and shared when not.
		 *
		 * TODO: indicate read-only sharing somewhere besides share_table.
		 */
		bool unmap = !VP_IS_COW(cur) && (VP_RIGHTS(cur) & L4_Writable) != 0;
		struct vp *copy = malloc(sizeof *copy);
		if(copy == NULL) goto Enomem;
		*copy = (struct vp){
			.vaddr = cur->vaddr | VPF_COW,
			.status = cur->status,
			.age = 1,
		};
		bool ok = htable_add(&dest->pages,
			hash_vp_by_vaddr(copy, NULL), copy);
		if(!ok) goto Enomem;
		if(unmap) {
			cur->vaddr |= VPF_COW;
			assert(VP_IS_COW(cur));
		}
		struct pp *phys = get_pp(cur->status);
		struct vp *prev_owner = NULL;
		if(!atomic_compare_exchange_strong_explicit(
			&phys->owner, &prev_owner, copy,
			memory_order_release, memory_order_relaxed))
		{
			/* multiple ownership. */
			ok = htable_add(&share_table, hash_vp_by_phys(copy, NULL), copy);
			if(!ok) {
				if(unmap) {
					cur->vaddr &= ~VPF_COW;
					assert(!VP_IS_COW(cur));
				}
				goto Enomem;
			}
		}

		if(unmap) {
			L4_Fpage_t phys = L4_FpageLog2(
				cur->status << PAGE_BITS, PAGE_BITS);
			L4_Set_Rights(&phys, L4_Writable);
			unmaps[unmap_pos++] = phys;
			if(unmap_pos == ARRAY_SIZE(unmaps)) {
				L4_UnmapFpages(ARRAY_SIZE(unmaps), unmaps);
				unmap_pos = 0;
			}
		}
	}

end:
	if(unmap_pos > 0) L4_UnmapFpages(unmap_pos, unmaps);
	// printf("forked %d pages (tail=%d).\n", n_pages, unmap_pos);
	return n_pages;

Enomem:
	n_pages = -ENOMEM;
	goto end;
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
	assert(destpid <= 0 || destpid == ra_ptr2id(vm_space_ra, dest));

	htable_init(&dest->pages, &hash_vp_by_vaddr, NULL);
	if(src == NULL) {
		/* creates an unconfigured address space as for spawn. caller should
		 * use VM::configure() to set it up before causing page faults.
		 */
		dest->kip_area.raw = ~0ul;
		dest->utcb_area = L4_Nilpage;
		dest->maps = RB_ROOT;
		dest->brk = 0;
	} else {
		/* fork from @src. */
		dest->kip_area = src->kip_area;
		dest->utcb_area = src->utcb_area;
		dest->sysinfo_area = src->sysinfo_area;
		dest->brk = src->brk;
		dest->maps = RB_ROOT;
		int n = fork_maps(src, dest);
		if(n == 0) n = fork_pages(src, dest);
		if(n < 0) {
			/* FIXME: cleanup */
			return n;
		}
	}

	return ra_ptr2id(vm_space_ra, dest);
}


static int vm_configure(
	L4_Word_t *last_resv_p,
	uint16_t pid, L4_Fpage_t utcb, L4_Fpage_t kip)
{
	if(pid > SNEKS_MAX_PID) return -EINVAL;
	struct vm_space *sp = ra_id2ptr(vm_space_ra, pid);
	if(sp->kip_area.raw == 0) return -EINVAL;
	if(sp->kip_area.raw != ~0ul || !L4_IsNilFpage(sp->utcb_area)) {
		/* TODO: should be "illegal state" */
		return -EINVAL;
	}
	if(L4_Address(kip) != L4_Address(utcb) + L4_Size(utcb)) return -EINVAL;
	/* FIXME: check for kip, utcb overlap with maps, pop errors if they do. */

	sp->kip_area = kip;
	sp->utcb_area = utcb;
	sp->sysinfo_area = L4_FpageLog2(
		L4_Address(kip) + L4_Size(kip), PAGE_BITS);

	*last_resv_p = L4_Address(sp->sysinfo_area) + L4_Size(sp->sysinfo_area) - 1;
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
	struct vp *old = htable_get(&sp->pages, hash, &cmp_vp_to_vaddr, &addr);
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
	assert(hash_vp_by_vaddr(v, NULL) == hash);
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


static bool has_shares(size_t hash, uint32_t page_num, int count)
{
	struct htable_iter it;
	for(struct vp *cur = htable_firstval(&share_table, &it, hash);
		cur != NULL;
		cur = htable_nextval(&share_table, &it, hash))
	{
		if(cur->status == page_num && --count <= 0) return true;
	}
	return false;
}


static L4_Fpage_t pf_cow(struct vp *virt)
{
	assert(VP_IS_COW(virt));
	struct pp *phys = get_pp(virt->status);
	struct vp *primary = atomic_load_explicit(&phys->owner,
		memory_order_relaxed);
	/* is @virt the sole owner of the page? */
	size_t hash = int_hash(virt->status);
	if(primary != virt) {
		/* unshare the secondary. */
		bool ok = htable_del(&share_table, hash, virt);
		if(!ok) {
			printf("%s: expected share, wasn't found???\n", __func__);
			abort();
		}
	}
	if((primary == virt && !has_shares(hash, virt->status, 1))
		|| (primary == NULL && !has_shares(hash, virt->status, 2)))
	{
		/* it is. take ownership. */
		if(primary == NULL) {
			atomic_store_explicit(&phys->owner, virt, memory_order_relaxed);
		}
		virt->vaddr &= ~VPF_COW;
	} else {
		/* no. make a copy. */
		if(primary == virt) {
			atomic_store_explicit(&phys->owner, NULL, memory_order_relaxed);
		}
		struct pl *newpl = get_free_pl();
		memcpy((void *)((uintptr_t)newpl->page_num << PAGE_BITS),
			(void *)((uintptr_t)virt->status << PAGE_BITS),
			PAGE_SIZE);
		virt->status = newpl->page_num;
		virt->vaddr &= ~VPF_COW;
		atomic_store_explicit(&pl2pp(newpl)->owner, virt,
			memory_order_release);
		push_page(&page_active_list, newpl);
		e_free(newpl);
	}

	L4_Fpage_t map_page = L4_FpageLog2(virt->status << PAGE_BITS, PAGE_BITS);
	L4_Set_Rights(&map_page, VP_RIGHTS(virt));
	return map_page;
}


/* TODO: add some kind of tracing enabler or printf-equivalent so that the
 * debug outputs get line buffering and so won't break TAP output. maybe.
 */
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

	const int fault_rwx = L4_Label(muidl_get_tag()) & 7;
#if TRACE_FAULTS
	printf("%s: pid=%d, faddr=%#lx, fip=%#lx, access=%c%c%c",
		__func__, pid, faddr, fip,
		(fault_rwx & L4_Readable) != 0 ? 'r' : '-',
		(fault_rwx & L4_Writable) != 0 ? 'w' : '-',
		(fault_rwx & L4_eXecutable) != 0 ? 'x' : '-');
#endif

	int eck = e_begin();

	L4_Fpage_t map_page;
	L4_Word_t faddr_page = faddr & ~PAGE_MASK;
	size_t hash = int_hash(faddr_page);
	struct vp *old = htable_get(&sp->pages, hash,
		&cmp_vp_to_vaddr, &faddr_page);
	if(old != NULL && old->status == 0) {
		/* lazy brk fastpath. */
#if TRACE_FAULTS
		printf("  lazy brk\n");
#endif
		struct pl *link = get_free_pl();
		void *page = (void *)((uintptr_t)link->page_num << PAGE_BITS);
		memset(page, '\0', PAGE_SIZE);
		map_page = L4_FpageLog2((L4_Word_t)page, PAGE_BITS);
		L4_Set_Rights(&map_page, VP_RIGHTS(old));
		old->status = link->page_num;
		atomic_store_explicit(&pl2pp(link)->owner, old, memory_order_relaxed);
		push_page(&page_active_list, link);
		e_free(link);
		goto reply;
	} else if(old != NULL && (VP_RIGHTS(old) & fault_rwx) != fault_rwx) {
#if TRACE_FAULTS
		printf("  no access!!!\n");
#endif
		printf("%s: segv (access=%#x, had=%#x)\n", __func__,
			fault_rwx, VP_RIGHTS(old));
		goto segv;
	} else if(old != NULL && VP_IS_COW(old)) {
		if((fault_rwx & L4_Writable) != 0) {
#if TRACE_FAULTS
			printf("  copy-on-write\n");
#endif
			map_page = pf_cow(old);
		} else {
			/* read-only while supplies last. */
#if TRACE_FAULTS
			printf("  read-only/shared\n");
#endif
			map_page = L4_FpageLog2(old->status << PAGE_BITS, PAGE_BITS);
			L4_Set_Rights(&map_page, fault_rwx);
		}
		goto reply;
	} else if(old != NULL) {
		/* quick remap or expand. */
#if TRACE_FAULTS
		printf("  remap\n");
#endif
		map_page = L4_FpageLog2(old->status << PAGE_BITS, PAGE_BITS);
		L4_Set_Rights(&map_page, VP_RIGHTS(old));
		goto reply;
	} else if(unlikely(ADDR_IN_FPAGE(sp->sysinfo_area, faddr))) {
#if TRACE_FAULTS
		printf("  sysinfopage\n");
#endif
		assert(the_sip->magic == SNEKS_SYSINFO_MAGIC);
		map_page = L4_FpageLog2((uintptr_t)the_sip, PAGE_BITS);
		L4_Set_Rights(&map_page, L4_Readable);
		goto reply;
	}

	int rights;
	struct lazy_mmap *mm = find_lazy_mmap(sp, faddr);
	if(mm == NULL) {
#if TRACE_FAULTS
		printf("  segv (unmapped)\n");
#endif
		goto segv;
	} else {
		assert(faddr >= mm->addr && faddr < mm->addr + mm->length);
		rights = (mm->flags >> 16) & 7;
		if(unlikely((fault_rwx & rights) != fault_rwx)) {
#if TRACE_FAULTS
			printf("  segv (access mode, mmap)\n");
#endif
			goto segv;
		}
	}

	struct vp *vp = malloc(sizeof *vp);
	if(unlikely(vp == NULL)) {
		/* FIXME */
		abort();
	}
	*vp = (struct vp){ .vaddr = faddr_page | rights };

	uint8_t *page;
	if((mm->flags & MAP_ANONYMOUS) != 0) {
#if TRACE_FAULTS
		printf("  anon\n");
#endif
		struct pl *link;
		link = get_free_pl();
		page = (uint8_t *)((uintptr_t)link->page_num << PAGE_BITS);
		memset(page, '\0', PAGE_SIZE);
		atomic_store_explicit(&pl2pp(link)->owner, vp, memory_order_relaxed);
		push_page(&page_active_list, link);
		e_free(link);
	} else {
#if TRACE_FAULTS
		printf("  file\n");
#endif
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
		atomic_store_explicit(&pl2pp(link)->owner, vp, memory_order_relaxed);
		push_page(&page_active_list, link);
		e_free(link);
	}

	vp->status = (L4_Word_t)page >> PAGE_BITS;
	bool ok = htable_add(&sp->pages, hash_vp_by_vaddr(vp, NULL), vp);
	if(unlikely(!ok)) {
		/* FIXME: roll back and segfault. */
		printf("%s: can't handle failing htable_add()!\n", __func__);
		abort();
	}

	map_page = L4_FpageLog2((L4_Word_t)page, PAGE_BITS);
	L4_Set_Rights(&map_page, rights);

reply:
	*map_out = L4_MapItem(map_page, faddr_page);
	e_end(eck);
	return;

segv:
	/* pop segfault and don't reply. */
	printf("%s: segfault in pid=%d at faddr=%#lx fip=%#lx\n",
		__func__, pid, faddr, fip);
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
		.configure = &vm_configure,
		.upload_page = &vm_upload_page,
		.breath_of_life = &vm_breath_of_life,
		.brk = &vm_brk,

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
