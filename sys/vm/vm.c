
/* systemspace POSIX-like virtual memory server.
 *
 * note that this is currently a single-threaded implementation, with some
 * lf/wf primitives mixed into the design even when they're not being used.
 * the idea is that once mung gets MP and muidl becomes multithreaded,
 * reworking vm to behave regardless of concurrency will be that much of a
 * smaller job, and that using epochs before MP should expose problems with
 * collection speed, usage issues, etc.
 */

#define VMIMPL_IMPL_SOURCE
//#define TRACE_FAULTS 1
//#define TRACE_FORK 1

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <ccan/likely/likely.h>
#include <ccan/compiler/compiler.h>
#include <ccan/minmax/minmax.h>
#include <ccan/array_size/array_size.h>
#include <ccan/container_of/container_of.h>
#include <ccan/darray/darray.h>
#include <ccan/bitmap/bitmap.h>

#include <l4/types.h>
#include <l4/ipc.h>
#include <l4/space.h>
#include <l4/kip.h>

#include <ukernel/rangealloc.h>
#include <ukernel/memdesc.h>
#include <sneks/mm.h>
#include <sneks/bitops.h>
#include <sneks/rbtree.h>
#include <sneks/process.h>
#include <sneks/hash.h>
#include <sneks/sysinfo.h>
#include <sneks/sanity.h>

#include "nbsl.h"
#include "epoch.h"
#include "defs.h"

#include "muidl.h"
#include "proc-defs.h"
#include "info-defs.h"
#include "fs-defs.h"
#include "vm-impl-defs.h"


#ifndef TRACE_FAULTS
#define TRACE_FAULT(...)
#else
#define TRACE_FAULT(...) printf(__VA_ARGS__)
#endif


struct pl;

typedef darray(struct pl *) plbuf;


/* flags etc. in vp->vaddr's low 12 bits:
 *   - VPM_RIGHTS covers L4.X2 access bits in the low 3. see VPF_COW.
 *   - VPF_SHARED: contents in the page cache.
 *   - VPF_ANON: contents not backed by a file.
 *   - VPF_COW write faults to this page are processed copy-on-write.
 *     exclusive with L4_Writable.
 */
#define VPM_RIGHTS 0x7
#define VPF_SHARED 0x8
#define VPF_ANON 0x10
#define VPF_COW 0x20


/* bitfield accessors. */
#define VP_RIGHTS(vp) ((vp)->vaddr & 7)	/* low 3 = rwx */
#define VP_IS_SHARED(vp) !!((vp)->vaddr & VPF_SHARED)
#define VP_IS_ANON(vp) !!((vp)->vaddr & VPF_ANON)
#define VP_IS_COW(vp) !!((vp)->vaddr & VPF_COW)

/* access of <struct pl>'s fsid_ino . */
#define PL_FSID(pl) ((pl)->fsid_ino >> 48)
#define PL_INO(pl) ((pl)->fsid_ino & ~0xffff000000000000ull)
#define PL_IS_PRIVATE(pl) ((pl)->fsid_ino == 0)
#define PL_IS_ANON(pl) (PL_FSID((pl)) == anon_fsid)

/* definition of the brk bloom filter in <struct vm_space>. */
#define BRK_BLOOM_WORDS (64 / sizeof(L4_Word_t))	/* one cacheline. */

#define IS_ANON_MMAP(mm) (((mm)->flags & MAP_ANONYMOUS) && ((mm)->flags & MAP_SHARED))


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
};


/* link to a <struct pp> by physical page number. */
struct pl {
	struct nbsl_node nn;
	uint32_t page_num;
	/* status indicates whether the link structure is valid (nonzero) or dead
	 * (zero). this is used in circumstances where a <struct pl> cannot be
	 * removed from its list by iterator, so it's useful to batch up several
	 * of them (such as in munmap_space()) and remove them in a single O(n)
	 * go.
	 */
	_Atomic uint32_t status;

	/* for pagecache pages: those backed by files and either shared or
	 * copy-on-written, and anonymous pages part of a shared-memory mapping.
	 *
	 * fsid_ino has the filesystem ID in the top 16 bits and a per-filesystem
	 * value in the lower 48, typically an inode number. fsid is pidof_NP() of
	 * filesystem, or vm for anonymous memory.
	 *
	 * these fields may be written in the structure received from
	 * get_free_pl(), because that structure will only be phantom up the
	 * freelist where these fields are ignored.
	 */
	uint64_t fsid_ino;
	uint32_t offset;	/* limits files to 16 TiB each */
};


/* virtual memory page. (there is no <struct vl>.)
 *
 * for now, ->status designates the physical page number used for this vpage.
 * its high bit should always be clear. there will be other formats for
 * swapspace slots, references to pagecache items, and so on. this arrangement
 * allows for as many as 2^31 physical pages in vm, or 8 TiB worth. if
 * ->status is 0, anonymous memory has not yet been attached to this page by
 *  the fault handler.
 *
 * flags are assigned in ->vaddr's low 12 bits as follows:
 *   - 2..0 are a mask of L4_Readable, L4_Writable, and L4_eXecutable,
 *     designating the access granted to the associated address space.
 *   - bit 3 indicates copy-on-write sharing.
 *   - bits 11..4 are not used and should be left clear.
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
	L4_Word_t brk_bloom[BRK_BLOOM_WORDS];
	L4_Fpage_t kip_area, utcb_area, sysinfo_area;
	struct htable pages;	/* <struct vp *> with hash_vp_by_vaddr() */
	struct rb_root maps;	/* lazy_mmap per range of addr and length */
	struct rb_root as_free;	/* as_free per non-overlapping ->fp */
	uintptr_t brk_lo, brk_hi; /* (excl) non-mmap anonymous private heap. */
	L4_Word_t mmap_bot;		/* bottom of as_free range */
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

	/* for pagecache access.
	 *
	 * when backed by a file, the three reference a filesystem service, an
	 * inode number, and a page offset within that file.
	 *
	 * when anonymous, ->fd_serv is VM's sysid, ->ino is zero for private maps
	 * and an anonymous mapping ID when the map is shared. offset is nonzero
	 * in tail fragment mmaps caused by munmap().
	 */
	L4_ThreadId_t fd_serv;
	uint64_t ino;
	size_t offset;
};


/* chunk of free address space in vm_space.as_free . */
struct as_free {
	struct rb_node rb;
	L4_Fpage_t fp;
};


#define flush_plbuf(pls) do { \
		remove_active_pls((pls)->item, (pls)->size); \
		darray_free(*(pls)); \
	} while(0)

/* tests whether [a, a+b) and [c, c+d) overlap.
 * TODO: move into an util.h somewhere.
 */
#define OVERLAP_EXCL(a, b, c, d) \
	({ typeof(a) __a = (a); typeof(c) __c = (c); \
	   !(__a + (b) <= __c || __a >= __c + (d)); \
	})


static size_t hash_vp_by_phys(const void *ptr, void *priv);
static size_t hash_lazy_mmap_by_ino(const void *ptr, void *priv);
static void remove_vp(struct vp *vp, plbuf *plbuf);
static void remove_active_pls(struct pl **pls, int n_pls);
static struct lazy_mmap *find_lazy_mmap(struct vm_space *sp, uintptr_t addr);
static void free_page(struct pl *link0, plbuf *plbuf);

static size_t pp_first, pp_total;
static struct rangealloc *pp_ra, *vm_space_ra;
static L4_Word_t user_addr_max;

static _Atomic unsigned long next_anon_ino = 1;
static unsigned short anon_fsid;	/* pidof_NP(L4_MyGlobalId()) */

/* all of these contain <struct pl> alone. */
static struct nbsl page_free_list = NBSL_LIST_INIT(page_free_list),
	page_active_list = NBSL_LIST_INIT(page_active_list);

/* multiset of vp by physical address when that physical page has been
 * referenced from more than one vp. a physical page's primary reference
 * (pp->owner) is always omitted, and may be NULL if it was removed before all
 * secondaries.
 */
static struct htable share_table = HTABLE_INITIALIZER(
	share_table, &hash_vp_by_phys, NULL);
/* similar but for ino of shared anonymous memory, i.e. PL_IS_ANON() of links
 * found in the page cache. mandatory for IS_ANON_MMAP(), forbidden to every
 * other lazy_mmap.
 */
static struct htable anon_mmap_table = HTABLE_INITIALIZER(
	anon_mmap_table, &hash_lazy_mmap_by_ino, NULL);

/* pagecache. */
static uint32_t pc_salt[4];	/* init-once salt for hash_cached_page() */
static struct nbsl *pc_buckets = NULL;
static unsigned char n_pc_buckets_log2;
#define n_pc_buckets (1u << n_pc_buckets_log2)

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


static size_t hash_lazy_mmap_by_ino(const void *ptr, void *priv) {
	const struct lazy_mmap *mm = ptr;
	return int_hash(mm->ino & 0xffffffff) ^ int_hash(mm->ino >> 32);
}


/* FIXME: this should assert that the pp's link field points to @link. a
 * conditional version can return NULL if it doesn't (i.e. to spot when the
 * physical page's ownership was contested, and the caller lost.)
 */
static inline struct pp *pl2pp(const struct pl *link) {
	assert(link->page_num >= pp_first);
	return ra_id2ptr(pp_ra, link->page_num - pp_first);
}


static struct pp *get_pp(uint32_t page_num) {
	assert(page_num >= pp_first);
	return ra_id2ptr(pp_ra, page_num - pp_first);
}


/* counts the number of occurrences of @page_num in share_table, returning
 * true if it's at least @count and false otherwise. @hash is
 * int_hash(@page_num), likely computed elsewhere already.
 */
static bool has_shares(size_t hash, uint32_t page_num, int count)
{
	assert(count > 0);
	struct htable_iter it;
	for(struct vp *cur = htable_firstval(&share_table, &it, hash);
		cur != NULL;
		cur = htable_nextval(&share_table, &it, hash))
	{
		if(cur->status == page_num && --count <= 0) return true;
	}
	return false;
}


#ifdef DEBUG_ME_HARDER
/* similar, but for anon_mmap_table, and computes hash by itself. */
static bool has_anon_mmaps(uint64_t ino, int count)
{
	assert(count > 0);
	struct lazy_mmap key = { .ino = ino };
	size_t hash = hash_lazy_mmap_by_ino(&key, NULL);
	struct htable_iter it;
	for(struct lazy_mmap *cur = htable_firstval(&anon_mmap_table, &it, hash);
		cur != NULL;
		cur = htable_nextval(&anon_mmap_table, &it, hash))
	{
		if(cur->ino == ino && --count <= 0) return true;
	}
	return false;
}
#endif


static int cmp_ptrs(const void *a, const void *b) {
	return *(const void **)a - *(const void **)b;
}


/* structural invariant checks.
 *
 * TODO: move these into invariant.c or some such, out of sight, and export
 * all the static globals properly.
 */

#ifndef DEBUG_ME_HARDER
#define invariants() true
#define vp_invariants(ctx, vp, pp, pl) true
#define all_space_invariants(ctx, allvps, nallvps) true
#define share_table_invariants(ctx, allvps, nallvps) true
#else
#include <sneks/invariant.h>
#include <setjmp.h>
#include <stdnoreturn.h>


/* copypasta'd via sys/crt, which vm doesn't link */
noreturn void longjmp(jmp_buf env, int val)
{
	extern noreturn void __longjmp_actual(jmp_buf, int);
	__longjmp_actual(env, val);
}


/* used by pc_invariants() */
static int cmp_vp_by_status(const void *a, const void *b) {
	return (long)(*(const struct vp **)a)->status
		- (long)(*(const struct vp **)b)->status;
}


static bool vp_invariants(INVCTX_ARG,
	struct vp *virt, struct pp *phys, struct pl *link)
{
	inv_iff1(virt->status == 0, phys == NULL);

	if(phys != NULL) {
		/* @virt is resident. */
		inv_ok1(~virt->status & 0x80000000);
		inv_ok1(virt->status == link->page_num);
	}

	inv_imply1(VP_IS_COW(virt), ~VP_RIGHTS(virt) & L4_Writable);
	inv_imply1(VP_RIGHTS(virt) & L4_Writable, !VP_IS_COW(virt));

	return true;

inv_fail:
	return false;
}


static bool all_space_invariants(INVCTX_ARG,
	struct vp *const *all_vps, size_t n_all_vps)
{
	assert(vm_space_ra != NULL);

	/* go over every space, and every vp therein. */
	struct ra_iter space_iter;
	for(struct vm_space *sp = ra_first(vm_space_ra, &space_iter);
		sp != NULL;
		sp = ra_next(vm_space_ra, &space_iter))
	{
		if(L4_IsNilFpage(sp->kip_area) || L4_IsNilFpage(sp->utcb_area)) {
			/* skip invalid and uninitialized spaces */
			continue;
		}
		inv_push("sp=%d, brk=[%#x, %#x)", ra_ptr2id(vm_space_ra, sp),
			sp->brk_lo, sp->brk_hi);

		/* (this all could be in a space_invariants(), one day.) */
		struct htable_iter vpit;
		for(const struct vp *vp = htable_first(&sp->pages, &vpit);
			vp != NULL;
			vp = htable_next(&sp->pages, &vpit))
		{
			/* the vp should be present in all_vps. */
			inv_ok(bsearch(&vp, all_vps, n_all_vps, sizeof(struct vp *),
				&cmp_ptrs) != NULL, "vp present in all_vps");

			uintptr_t vaddr = vp->vaddr & ~PAGE_MASK;
			inv_log("vaddr=%#x", vaddr);
			/* should lay within the brk range or in a lazy_mmap. */
			inv_ok((vaddr >= sp->brk_lo && vaddr < sp->brk_hi)
					|| find_lazy_mmap(sp, vaddr) != NULL,
				"vp->vaddr within brk range or lazy_mmap");
		}

		inv_log("kip_area=%#lx:%#lx, utcb_area=%#lx:%#lx, sysinfo_area=%#lx:%#lx",
			L4_Address(sp->kip_area), L4_Size(sp->kip_area),
			L4_Address(sp->utcb_area), L4_Size(sp->utcb_area),
			L4_Address(sp->sysinfo_area), L4_Size(sp->sysinfo_area));
		inv_ok1(!fpage_overlap(sp->kip_area, sp->utcb_area));
		inv_ok1(!fpage_overlap(sp->kip_area, sp->sysinfo_area));
		inv_ok1(!fpage_overlap(sp->utcb_area, sp->sysinfo_area));

		inv_pop();
	}

	return true;

inv_fail:
	return false;
}


static bool share_table_invariants(INVCTX_ARG,
	struct vp *const *all_vps, size_t n_all_vps)
{
	/* all <struct vp *> found through share_table, including the ones that're
	 * actually the sole owner (i.e. where pp->owner == NULL).
	 */
	darray(struct vp *) share_vps = darray_new(),
		owner_vps = darray_new();	/* ones discovered thru pp->owner */

	struct htable_iter it;
	for(const struct vp *share = htable_first(&share_table, &it);
		share != NULL;
		share = htable_next(&share_table, &it))
	{
		inv_push("share=%p: ->vaddr=%#x, ->status=%#x, ->age=%u",
			share, share->vaddr, share->status, share->age);

		inv_ok1(~share->status & 0x80000000);
		if(all_vps != NULL) {
			inv_ok(bsearch(&share, all_vps, n_all_vps, sizeof(struct vp *),
				&cmp_ptrs) != NULL, "share present in all_vps");
		}

		const struct pp *phys = get_pp(share->status);
		inv_log("phys=%p: ->link=%p, ->owner=%p",
			phys, phys->link, phys->owner);
		inv_ok1(phys->link->page_num == share->status);
		inv_imply1(phys->owner != NULL,
			phys->owner->status == share->status);
		inv_ok1(share != phys->owner);
		if(all_vps != NULL) {
			inv_imply1(phys->owner != NULL,
				bsearch(&phys->owner, all_vps, n_all_vps,
					sizeof(struct vp *), &cmp_ptrs) != NULL);
		}

		if(phys->owner != NULL) darray_push(owner_vps, phys->owner);
		darray_push(share_vps, (struct vp *)share);

		inv_pop();
	}

	/* sort and uniq owner_vps, since that'll have as many duplicates of a
	 * primary owner as there are matching secondaries in share_table. then
	 * add it to share_vps to top that one up.
	 *
	 * also check that owner_vps and share_table are disjoint.
	 */
	qsort(owner_vps.item, owner_vps.size, sizeof(void *), &cmp_ptrs);
	qsort(share_vps.item, share_vps.size, sizeof(void *), &cmp_ptrs);
	size_t o = 0;
	for(size_t i=1; i < owner_vps.size; i++) {
		assert(o < i);
		if(o == 0 || owner_vps.item[o - 1] != owner_vps.item[i]) {
			owner_vps.item[o++] = owner_vps.item[i];
			inv_ok(bsearch(&owner_vps.item[i],
				share_vps.item, share_vps.size, sizeof(struct vp *),
				&cmp_ptrs) == NULL, "owner_vps and share_vps are disjoint");
		}
		assert(o < owner_vps.alloc || i == owner_vps.size - 1);
	}
	darray_resize(owner_vps, o);
	darray_append_items(share_vps, owner_vps.item, owner_vps.size);

	/* sort share_vps and check against duplicates. */
	qsort(share_vps.item, share_vps.size, sizeof(void *), &cmp_ptrs);
	inv_imply1(all_vps != NULL, share_vps.size <= n_all_vps);
	for(size_t i=1; i < share_vps.size; i++) {
		inv_push("share_vps[%d]=%p ∧ share_vps[i=%d]=%p",
			i-1, share_vps.item[i-1], i, share_vps.item[i]);
		inv_log("->status=%#x", share_vps.item[i]->status);
		inv_ok1(share_vps.item[i - 1] != share_vps.item[i]);
		inv_pop();
	}

	darray_free(share_vps);
	darray_free(owner_vps);
	return true;

inv_fail:
	darray_free(share_vps);
	darray_free(owner_vps);
	return false;
}


static bool pc_invariants(INVCTX_ARG,
	struct vp *const *all_vps, size_t n_all_vps)
{
	struct vp **vps = malloc(sizeof *vps * n_all_vps);
	memcpy(vps, all_vps, sizeof *vps * n_all_vps);
	qsort(vps, n_all_vps, sizeof *vps, &cmp_vp_by_status);
	inv_ok1(n_all_vps < 2 || vps[0]->status <= vps[1]->status);

	for(int i=0; i < n_pc_buckets; i++) {
		struct nbsl *list = &pc_buckets[i];
		struct nbsl_iter it;
		for(struct nbsl_node *cur = nbsl_first(list, &it);
			cur != NULL;
			cur = nbsl_next(list, &it))
		{
			struct pl *link = container_of(cur, struct pl, nn);
			if(atomic_load(&link->status) == 0) continue;	/* skip garbage */
			size_t hash = int_hash(link->page_num);
			inv_push("link=%p: ->page_num=%#x [pl]->owner=%p",
				link, link->page_num, pl2pp(link)->owner);

			/* anon+shared pages should be removed eagerly as lazy_mmaps get
			 * removed or reshaped.
			 */
			inv_imply1(PL_IS_ANON(link), has_anon_mmaps(PL_INO(link), 1));

			struct vp key = { .status = link->page_num }, *keyptr = &key,
				**vpp = bsearch(&keyptr, vps, n_all_vps, sizeof *vps,
					&cmp_vp_by_status);
			inv_imply1(vpp == NULL, pl2pp(link)->owner == NULL);
			inv_imply1(vpp == NULL, !has_shares(hash, link->page_num, 1));
			inv_imply1(vpp != NULL,
				pl2pp(link)->owner == *vpp
					|| has_shares(hash, link->page_num, 1));
			if(vpp != NULL) {
				/* wind it back. */
				struct vp *virt = *vpp;
				while(vpp > vps && vpp[-1]->status == virt->status) {
					virt = *--vpp;
				}

				/* then iterate them all. */
				int nth = 0;
				do {
					inv_log("virt[%d]=%p, ->vaddr=%#x, ->status=%#x",
						nth++, virt, virt->vaddr, virt->status);
					inv_ok1(!VP_IS_ANON(virt));
					inv_ok1(VP_IS_SHARED(virt));
				} while(vpp < vps + n_all_vps
					&& (virt = *++vpp, vpp[-1]->status == virt->status));
			}

			inv_pop();
		}
	}

	free(vps);
	return true;

inv_fail:
	free(vps);
	return false;
}


static bool invariants(void)
{
	INV_CTX;
	int eck = e_begin();

	/* TODO: wait until all other threads in sys/vm have gone idle or hit this
	 * point also. but there ain't no whales so we tell tall tales and sing
	 * our whaling tune.
	 */

	/* every physical page should be found exactly once in the free list,
	 * active list, or page cache.
	 */
	bitmap *phys_seen = bitmap_alloc0(pp_total);
	darray(struct vp *) all_vps = darray_new();
	for(int i=0; i < 2 + (pc_buckets != NULL ? n_pc_buckets : 0); i++) {
		struct nbsl *list;
		switch(i) {
			case 0: list = &page_free_list; break;
			case 1: list = &page_active_list; break;
			default: list = &pc_buckets[i - 2]; break;
		}
		inv_push("list=%p (i=%d)", list, i);
		struct nbsl_iter it;
		for(struct nbsl_node *cur = nbsl_first(list, &it);
			cur != NULL;
			cur = nbsl_next(list, &it))
		{
			struct pl *link = container_of(cur, struct pl, nn);
			if(atomic_load(&link->status) == 0) continue;	/* skip garbage */
			inv_push("link=%p, ->page_num=%u", link, link->page_num);
			struct pp *phys = pl2pp(link);
			inv_ok1(phys->link == link);

			inv_ok1(link->page_num >= pp_first);
			size_t pgix = link->page_num - pp_first;
			inv_ok1(!bitmap_test_bit(phys_seen, pgix));
			bitmap_set_bit(phys_seen, pgix);

			/* physical page ownership. */
			inv_imply1(list == &page_free_list, phys->owner == NULL);
			size_t pnhash = int_hash(link->page_num);
			inv_imply1(list == &page_active_list,
				phys->owner != NULL || has_shares(pnhash, link->page_num, 1));

			/* checks on and collection of virtual memory pages, per phys
			 * tracking.
			 */
			inv_imply1(phys->owner != NULL,
				vp_invariants(INV_CHILD, phys->owner, phys, link));
			if(phys->owner != NULL) darray_push(all_vps, phys->owner);
			struct htable_iter hit;
			for(struct vp *vp = htable_firstval(&share_table, &hit, pnhash);
				vp != NULL;
				vp = htable_nextval(&share_table, &hit, pnhash))
			{
				if(vp->status != link->page_num) continue;
				inv_ok1(vp_invariants(INV_CHILD, vp, phys, link));
				darray_push(all_vps, vp);
			}

			inv_pop();
		}
		inv_pop();
	}
	inv_ok(bitmap_full(phys_seen, pp_total),
		"pp_total=%u pp seen", (unsigned)pp_total);

	/* sort all_vps and check that each occurs just once. */
	qsort(all_vps.item, all_vps.size, sizeof(void *), &cmp_ptrs);
	for(size_t i=1; i < all_vps.size; i++) {
		inv_push("all_vps[i=%d]=%p", i, all_vps.item[i]);
		inv_log("->status=%#x", all_vps.item[i]->status);
		inv_ok1(all_vps.item[i - 1] != all_vps.item[i]);
		inv_pop();
	}
	if(vm_space_ra != NULL) {
		inv_ok1(all_space_invariants(INV_CHILD, all_vps.item, all_vps.size));
	}

	inv_ok1(share_table_invariants(INV_CHILD, all_vps.item, all_vps.size));
	inv_ok1(pc_buckets == NULL
		|| pc_invariants(INV_CHILD, all_vps.item, all_vps.size));

	e_end(eck);
	free(phys_seen);
	darray_free(all_vps);
	return true;

inv_fail:
	e_end(eck);
	free(phys_seen);
	darray_free(all_vps);
	return false;
}

#endif


/* doesn't discard @oldlink, or mark it for disposal. */
static void push_page(struct nbsl *list, struct pl *oldlink)
{
	struct pl *nl = malloc(sizeof *nl);
	*nl = *oldlink;
	atomic_store_explicit(&nl->status, 1, memory_order_relaxed);	/* look alive */
	atomic_store_explicit(&pl2pp(oldlink)->link, nl, memory_order_relaxed);
	struct nbsl_node *top;
	do {
		top = nbsl_top(list);
	} while(!nbsl_push(list, top, &nl->nn));
}


/* shooting a fly with a cannon, here */
static size_t hash_cached_page(uint32_t a, uint32_t b, uint32_t c)
{
	return bob96bitmix(
		bob96bitmix(pc_salt[0], pc_salt[1], a),
		bob96bitmix(pc_salt[2], pc_salt[3], b),
		c);
}

static size_t hash_lazy_mmap(const struct lazy_mmap *mm, int bump)
{
	return hash_cached_page(mm->offset + bump,
		pidof_NP(mm->fd_serv) << 16 | ((mm->ino >> 32) & 0xffff),
		mm->ino & 0xffffffffu);
}


/* insert copy of @oldlink under @lookup_top, or find an existing link, but
 * either way stash it in *@cached_p.
 *
 * TODO: recycle the hash value from find_cached_page() as well.
 */
static int push_cached_page(
	struct pl **cached_p,
	struct nbsl_node *lookup_top, const struct pl *oldlink)
{
	size_t hash = hash_cached_page(oldlink->offset,
		oldlink->fsid_ino >> 32, oldlink->fsid_ino & 0xffffffffu);
	struct nbsl *list = &pc_buckets[hash & (n_pc_buckets - 1)];

	struct pl *nl = malloc(sizeof *nl);
	if(nl == NULL) return -ENOMEM;
	*nl = *oldlink;
	atomic_store_explicit(&nl->status, 1, memory_order_relaxed);
	do {
		struct nbsl_node *top = nbsl_top(list);
		if(lookup_top != top) {
			/* examine new nodes between first and @lookup_top. */
			struct nbsl_iter it;
			for(struct nbsl_node *cur = nbsl_first(list, &it);
				cur != NULL && cur != lookup_top;
				cur = nbsl_next(list, &it))
			{
				struct pl *cand = container_of(cur, struct pl, nn);
				if(cand->fsid_ino == nl->fsid_ino
					&& cand->offset == nl->offset)
				{
					*cached_p = cand;
					free(nl);
					return -EEXIST;
				}
			}
			lookup_top = top;
		}
	} while(!nbsl_push(list, lookup_top, &nl->nn));

	atomic_store(&pl2pp(oldlink)->link, nl);
	*cached_p = nl;

	return 0;
}


static struct pl *find_cached_page(
	struct nbsl_node **top_p,
	const struct lazy_mmap *mm, int bump)
{
	assert(top_p != NULL);

	size_t hash = hash_lazy_mmap(mm, bump);
	struct nbsl *bucket = &pc_buckets[hash & (n_pc_buckets - 1)];
	struct nbsl_iter it;
	for(struct nbsl_node *cur = *top_p = nbsl_first(bucket, &it);
		cur != NULL;
		cur = nbsl_next(bucket, &it))
	{
		struct pl *cand = container_of(cur, struct pl, nn);
		if(atomic_load_explicit(&cand->status, memory_order_acquire) > 0
			&& PL_INO(cand) == mm->ino
			&& cand->offset == mm->offset + bump
			&& PL_FSID(cand) == pidof_NP(mm->fd_serv))
		{
			/* booya! */
			return cand;
		}
	}

	return NULL;
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


/* the silly bruteforce edit. */
static struct lazy_mmap *first_lazy_mmap(
	struct vm_space *sp, uintptr_t addr, size_t sz)
{
	assert(VALID_ADDR_SIZE(addr, sz));
	struct rb_node *n = sp->maps.rb_node;
	struct lazy_mmap *cand = NULL;
	while(n != NULL) {
		cand = rb_entry(n, struct lazy_mmap, rb);
		if(addr < cand->addr) n = n->rb_left;
		else if(addr >= cand->addr + cand->length) n = n->rb_right;
		else return cand;
	}
	/* >> */
	while(cand != NULL && addr + sz <= cand->addr) {
		cand = container_of_or_null(__rb_next(&cand->rb),
			struct lazy_mmap, rb);
	}
	/* << */
	while(cand != NULL && addr >= cand->addr + cand->length) {
		cand = container_of_or_null(__rb_prev(&cand->rb),
			struct lazy_mmap, rb);
	}
	/* \o/ */
	return cand;
}


static COLD void init_phys(L4_Fpage_t *phys, int n_phys)
{
	size_t p_min = ~0ul, p_max = 0;
	for(int i=0; i < n_phys; i++) {
		p_min = min_t(size_t, p_min, L4_Address(phys[i]) >> PAGE_BITS);
		p_max = max_t(size_t, p_max,
			(L4_Address(phys[i]) + L4_Size(phys[i])) >> PAGE_BITS);
	}
	pp_total = p_max - p_min;
	printf("vm: allocating %lu <struct pp> (first is %lu)\n",
		(unsigned long)pp_total, (unsigned long)p_min);
	pp_first = p_min;

	pp_ra = RA_NEW(struct pp, pp_total);
	for(int i=0; i < n_phys; i++) {
		int base = L4_Address(phys[i]) >> PAGE_BITS;
		assert(base > 0);
		for(int o=0; o < L4_Size(phys[i]) >> PAGE_BITS; o++) {
			struct pp *pp = ra_alloc(pp_ra, base + o - pp_first);
			struct pl *link = malloc(sizeof *link);
			*link = (struct pl){ .page_num = base + o, .status = 1 };
			atomic_store(&pp->link, link);
			struct nbsl_node *top;
			do {
				top = nbsl_top(&page_free_list);
			} while(!nbsl_push(&page_free_list, top, &link->nn));
			assert(pl2pp(link) == pp);
			assert(pp->link == link);
		}
	}
	printf("vm: physical memory tracking initialized.\n");
}


static COLD void init_pc(const L4_Fpage_t *phys, int n_phys)
{
	uint64_t total_pages = 0;
	for(int i=0; i < n_phys; i++) {
		total_pages += L4_Size(phys[i]) / PAGE_SIZE;
	}

	/* pagecache bucket heads cost two words apiece, so we'll allocate one for
	 * every 12 physical pages -- rounding up to nearest power of two.
	 */
	n_pc_buckets_log2 = size_to_shift(total_pages / 12);
	pc_buckets = malloc(sizeof *pc_buckets * n_pc_buckets);
	if(pc_buckets == NULL) {
		printf("vm: can't allocate pagecache buckets???\n");
		abort();
	}
	for(int i=0; i < n_pc_buckets; i++) nbsl_init(&pc_buckets[i]);

	/* TODO: grab these off a random number generator when NDEBUG is defined,
	 * but use stupid constants otherwise.
	 */
	pc_salt[0] = 0xdeadbeef;
	pc_salt[1] = 0xcafebabe;
	pc_salt[2] = 0xfeedface;
	pc_salt[3] = 0x12345678;

	printf("vm: page cache initialized with 2**%d (= %u) buckets\n",
		n_pc_buckets_log2, n_pc_buckets);
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


static L4_Word_t get_as_free(struct vm_space *sp, size_t length)
{
	/* TODO: search through sp->as_free in reverse for a chunk that fits
	 * @length & carve it up.
	 */
	return 0;
}


/* remove @mm's contents from the page cache that lie between [first, last).
 * to remove it entirely, pass first=0 ∧ last=0. @mm will not be removed from
 * anon_mmap_table.
 *
 * NOTE: this should go away along with all its callsites when page
 * replacement comes in. at that point anon+shared pages in the page cache
 * will be cleaned up lazily based on anon_mmap_table, which is so cool as
 * to permit an interim bruteforce solution for passing invariant checks.
 */
static void cut_anon_mmap(
	struct lazy_mmap *mm, size_t hash, uintptr_t first, uintptr_t last)
{
	assert(IS_ANON_MMAP(mm));
	assert(hash == hash_lazy_mmap_by_ino(mm, NULL));

	if(last == first) last = mm->addr + mm->length - 1;
	else last = min(mm->addr + mm->length - 1, last);
	first = max(mm->addr, first);

	/* brute force. 's cool */
	plbuf pls = darray_new();
	for(int page=0; page < (last - first + 1) >> PAGE_BITS; page++) {
		struct nbsl_node *top;
		struct pl *link = find_cached_page(&top, mm, mm->offset + page);
		if(link == NULL) continue;

		/* is this one covered by the remaining anon mmaps on this ino? */
		bool cover = false;
		struct htable_iter it;
		for(struct lazy_mmap *oth = htable_firstval(&anon_mmap_table, &it, hash);
			oth != NULL;
			oth = htable_nextval(&anon_mmap_table, &it, hash))
		{
			if(oth->ino != mm->ino || oth == mm) continue;
			if(OVERLAP_EXCL(first + page * PAGE_SIZE, PAGE_SIZE,
				oth->addr, oth->length))
			{
				cover = true;
				break;
			}
		}

		if(!cover) {
			assert(!has_shares(int_hash(link->page_num), link->page_num, 1));
			assert(pl2pp(link)->owner == NULL);
			free_page(link, &pls);
		}
	}
	flush_plbuf(&pls);
}


/* split and remove lazy mmaps and vpages covered by @addr:@size. */
static void munmap_space(struct vm_space *sp, L4_Word_t addr, size_t size)
{
	assert(e_inside());
	assert(((addr | size) & PAGE_MASK) == 0);
	assert(VALID_ADDR_SIZE(addr, size));

	/* remove virtual pages. */
	plbuf pls = darray_new();
	for(L4_Word_t pos = addr; pos < addr + size; pos += PAGE_SIZE) {
		size_t hash = int_hash(pos);
		/* WIBNI there were a htable_get() that returned a copy of the
		 * iterator that a subsequent htable_del() needn't iterate again?
		 */
		struct vp *v = htable_get(&sp->pages, hash, &cmp_vp_to_vaddr, &pos);
		if(v == NULL) continue;
		htable_del(&sp->pages, hash, v);
		remove_vp(v, &pls);
		assert(htable_get(&sp->pages, hash, &cmp_vp_to_vaddr, &pos) == NULL);
	}
	flush_plbuf(&pls);

	/* carve up lazy mmaps */
	for(struct lazy_mmap *cur = first_lazy_mmap(sp, addr, size), *next;
		cur != NULL;
		cur = next)
	{
		next = container_of_or_null(__rb_next(&cur->rb),
			struct lazy_mmap, rb);
		assert(cur->addr >= addr || cur->addr + cur->length > addr);
		if(cur->addr >= addr + size) break;
		size_t hash = hash_lazy_mmap_by_ino(cur, NULL);
		if(cur->addr >= addr && cur->addr + cur->length <= addr + size) {
			/* lazy_mmap entirely within range. remove it outright. */
			if(IS_ANON_MMAP(cur)) {
				cut_anon_mmap(cur, hash, 0, 0);
				if(!htable_del(&anon_mmap_table, hash, cur)) {
					printf("vm:%s: lazy mmap mm=%p not in htable?\n",
						__func__, cur);
				}
			}
			__rb_erase(&cur->rb, &sp->maps);
			e_free(cur);
			continue;
		} else if(cur->addr < addr && cur->addr + cur->length > addr + size) {
			/* the other way around; shorten and add fragment for tail. */
			struct lazy_mmap *tail = malloc(sizeof *tail);
			if(tail == NULL) goto Enomem;
			*tail = *cur;	/* TODO: duplicate fds? */
			tail->addr = addr + size;
			tail->length = cur->length - (tail->addr - cur->addr);
			cur->length = addr - cur->addr;
			struct lazy_mmap *old = insert_lazy_mmap(sp, tail);
			assert(old == NULL);
			if(IS_ANON_MMAP(cur)) {
				cut_anon_mmap(cur, hash, cur->addr + cur->length, tail->addr - 1);
				bool ok = htable_add(&anon_mmap_table, hash, tail);
				if(!ok) goto Enomem;
			}
		} else if(cur->addr < addr) {
			/* overlap on the left. shorten existing map. */
			assert(addr < cur->addr + cur->length);
			cur->length = addr - cur->addr;
			if(IS_ANON_MMAP(cur)) {
				cut_anon_mmap(cur, hash, cur->addr,
					cur->addr + cur->length - 1);
			}
		} else {
			/* overlap on the right. move existing map up. */
			assert(cur->addr + cur->length > addr + size);
			assert(cur->addr < addr + size);
			if(IS_ANON_MMAP(cur)) {
				cut_anon_mmap(cur, hash, cur->addr, addr - 1);
			}
			cur->length -= addr + size - cur->addr;
			cur->addr = addr + size;
		}
	}

	assert(first_lazy_mmap(sp, addr, size) == NULL);
	return;

Enomem:
	/* FIXME: handle this by propagating ENOMEM to the caller. the goal is
	 * idempotence but not atomicity; the caller is expected to resolve this
	 * by suspending execution until more heap space becomes available.
	 */
	printf("vm:%s: out of memory\n", __func__);
	abort();
}


/* TODO: expand existing mmaps even when @fixed is set. */
static int reserve_mmap(
	struct lazy_mmap *mm,
	struct vm_space *sp, L4_Word_t address, size_t length,
	bool fixed)
{
	assert((length & PAGE_MASK) == 0);
	assert((address & PAGE_MASK) == 0);

	/* NOTE: instead of checking these for every mmap, we could allow
	 * lazy_mmaps over sip, kip, and utcb but render them ineffective in
	 * vm_pf() (and therefore vm_munmap()). this saves us a _NP return
	 * from mmap(2).
	 */
	if((sp->brk_lo != sp->brk_hi
			&& OVERLAP_EXCL(sp->brk_lo, sp->brk_hi - sp->brk_lo,
				address, length))
		|| OVERLAP_EXCL(L4_Address(sp->utcb_area), L4_Size(sp->utcb_area),
			address, length)
		|| OVERLAP_EXCL(L4_Address(sp->kip_area), L4_Size(sp->kip_area),
			address, length)
		|| OVERLAP_EXCL(L4_Address(sp->sysinfo_area),
			L4_Size(sp->sysinfo_area), address, length))
	{
		/* bad hint. bad! */
		if(fixed) return -EEXIST; else address = 0;
	}

	if(IS_ANON_MMAP(mm)) {
		bool ok = htable_add(&anon_mmap_table,
			hash_lazy_mmap_by_ino(mm, NULL), mm);
		if(!ok) return -ENOMEM;
	}

	struct lazy_mmap *old;
	mm->addr = address;
	mm->length = length;
	if(fixed) {
		old = insert_lazy_mmap(sp, mm);
		if(old != NULL) {
			munmap_space(sp, mm->addr, mm->length);
			old = insert_lazy_mmap(sp, mm);
		}
	} else if(address == 0 || (old = insert_lazy_mmap(sp, mm)) != NULL) {
		/* TODO: use @address hint to find something "nearby". */
		address = get_as_free(sp, mm->length);
		if(address != 0) mm->addr = address;
		else {
			/* carve up more fresh address space. mm, delicious. */
			/* FIXME: avoid utcb, kip, sysinfo */
			mm->addr = sp->mmap_bot - mm->length + 1;
			sp->mmap_bot -= mm->length;
		}
		old = insert_lazy_mmap(sp, mm);
	}

	/* move brk_{lo,hi} up to assist uninitialized spaces (spawn, exec). */
	if(sp->brk_lo == sp->brk_hi) {
		sp->brk_lo = sp->brk_hi = max_t(uintptr_t,
			sp->brk_hi, (mm->addr + length + PAGE_SIZE - 1) & ~PAGE_MASK);
	}

	assert(old == NULL);
	return 0;
}


static int vm_mmap(
	uint16_t target_pid, L4_Word_t *addr_ptr, uint32_t length,
	int prot, int flags,
	L4_Word_t fd_serv, L4_Word_t fd, uint32_t offset)
{
	assert(invariants());

	if(length == 0) return -EINVAL;
	if((*addr_ptr | offset) & PAGE_MASK) return -EINVAL;
	if((flags & MAP_PRIVATE) && (flags & MAP_SHARED)) return -EINVAL;
	if((~flags & MAP_PRIVATE) && (~flags & MAP_SHARED)) return -EINVAL;

	int sender_pid = pidof_NP(muidl_get_sender());
	if(target_pid == 0) target_pid = sender_pid;
	if(target_pid > SNEKS_MAX_PID || target_pid == 0) return -EINVAL;

	struct vm_space *sp = ra_id2ptr(vm_space_ra, target_pid);
	if(sp->kip_area.raw == 0) return -EINVAL;

	struct lazy_mmap *mm = malloc(sizeof *mm);
	if(mm == NULL) return -ENOMEM;
	if((flags & MAP_ANONYMOUS) && (flags & MAP_SHARED)) {
		/* (error if fd_serv != nil?) */
		fd_serv = L4_MyGlobalId().raw;
		fd = atomic_fetch_add(&next_anon_ino, 1);
		offset = 0;
	} else if((~flags & MAP_ANONYMOUS) && fd_serv != L4_nilthread.raw) {
		/* TODO: validate fd_serv somehow */
	}
	*mm = (struct lazy_mmap){
		.flags = prot_to_l4_rights(prot) << 16 | (flags & ~MAP_FIXED),
		.fd_serv.raw = fd_serv, .ino = fd, .offset = offset >> PAGE_BITS,
	};
	int n = reserve_mmap(mm, sp, *addr_ptr,
		(length + PAGE_SIZE - 1) & ~PAGE_MASK, !!(flags & MAP_FIXED));
	if(n < 0) {
		free(mm);
		goto end;
	}

	assert(~mm->flags & MAP_FIXED);
	*addr_ptr = mm->addr;

end:
	assert(invariants());
	return n;
}


static int vm_brk(L4_Word_t addr)
{
	assert(invariants());

	int sender_pid = pidof_NP(muidl_get_sender());
	if(unlikely(sender_pid >= SNEKS_MAX_PID)) return -EINVAL;

	struct vm_space *sp = ra_id2ptr(vm_space_ra, sender_pid);
	assert(!L4_IsNilFpage(sp->utcb_area));

	if(addr > user_addr_max) return -ENOMEM;
	if(addr < sp->brk_lo) return -ENOMEM;	/* a bit odd */

	/* round up because sbrk() adds bytes. */
	uintptr_t newhi = (addr + PAGE_SIZE - 1) & ~PAGE_MASK;
	if(newhi < sp->brk_hi) {
		int eck = e_begin();
		munmap_space(sp, newhi, sp->brk_hi - newhi);
		e_end(eck);
	} else if(newhi > sp->brk_hi
		&& first_lazy_mmap(sp, sp->brk_hi, newhi - sp->brk_hi) != NULL)
	{
		/* overlaps mmaps */
		return -ENOMEM;
	}

	assert(invariants());
	sp->brk_hi = newhi;
	return 0;
}


static int vm_erase(unsigned short target_pid)
{
	assert(invariants());

	int sender_pid = pidof_NP(muidl_get_sender());
	if(unlikely(sender_pid < SNEKS_MIN_SYSID)) return -EPERM;

	struct vm_space *sp = ra_id2ptr(vm_space_ra, target_pid);
	if(L4_IsNilFpage(sp->utcb_area) || L4_IsNilFpage(sp->kip_area)) {
		assert(sp->pages.elems == 0);
		assert(__rb_first(&sp->maps) == NULL);
		return -EINVAL;
	}

	int eck = e_begin();

	/* toss free address space tracking bits */
	RB_FOREACH_SAFE(cur, &sp->as_free) {
		__rb_erase(cur, &sp->as_free);
		free(rb_entry(cur, struct as_free, rb));
	}

	/* and the process virtual memory */
	L4_Fpage_t fps[64];
	int n_fps = 0;
	plbuf pls = darray_new();
	struct htable_iter it;
	for(struct vp *v = htable_first(&sp->pages, &it);
		v != NULL;
		v = htable_next(&sp->pages, &it))
	{
		/* TODO: this isn't nice: it's slower than a destroying
		 * L4_ThreadControl() on account of the multiple syscalls in a
		 * nontrivial space, it'll needlessly unmap laterally related virtual
		 * memory, and the call to unmap loses access data used by page
		 * replacement.
		 *
		 * all of these are minor issues until benchmarks and page replacement
		 * get implemented.
		 *
		 * the solutions are 1) collecting <struct pp>, sorting them by
		 * physical address, and passing them in a cache-favouring order to
		 * L4_Unmap; 2) sucking up and dealing with it; and 3) storing a copy
		 * of the access bits in <struct pp>.
		 */
		assert(~v->status & 0x80000000);
		L4_Fpage_t fp = L4_FpageLog2(v->status << PAGE_BITS, PAGE_BITS);
		L4_Set_Rights(&fp, L4_FullyAccessible);
		fps[n_fps++] = fp;
		if(n_fps == ARRAY_SIZE(fps)) {
			/* drop the bass */
			L4_UnmapFpages(ARRAY_SIZE(fps), fps);
			n_fps = 0;
		}

		remove_vp(v, &pls);
	}
	if(n_fps > 0) L4_UnmapFpages(n_fps, fps);
	htable_clear(&sp->pages);
	flush_plbuf(&pls);

	/* and finally the lazy mmaps */
	RB_FOREACH_SAFE(cur, &sp->maps) {
		struct lazy_mmap *mm = rb_entry(cur, struct lazy_mmap, rb);
		if(mm->fd_serv.raw == L4_MyGlobalId().raw) {
			/* anonymous shared memory. */
			assert(IS_ANON_MMAP(mm));
			size_t hash = hash_lazy_mmap_by_ino(mm, NULL);
			cut_anon_mmap(mm, hash, 0, 0);
			if(!htable_del(&anon_mmap_table, hash, mm)) {
				printf("vm:%s: anon shared mm=%p not in htable?\n",
					__func__, mm);
			}
		} else if(!L4_IsNilThread(mm->fd_serv)) {
			/* TODO: unref the inode on the filesystem, as appropriate */
			assert(!IS_ANON_MMAP(mm));
		}
		__rb_erase(&mm->rb, &sp->maps);
		e_free(mm);
	}

	sp->kip_area = sp->utcb_area = sp->sysinfo_area = L4_Nilpage;
	ra_free(vm_space_ra, sp);
	e_end(eck);

	assert(invariants());
	return 0;
}


/* returns as htable_add().
 * TODO: why does this use @status instead of @share->status?
 */
static bool add_share(uint32_t status, struct vp *share)
{
	/* TODO: handle other forms of vp->status as they appear. */
	assert(~status & 0x80000000);	/* in physical memory */
	struct pp *phys = get_pp(status);
	struct vp *prev_owner = NULL;
	if(atomic_compare_exchange_strong(&phys->owner, &prev_owner, share)) {
		/* primary ownership taken. */
		return true;
	} else {
		/* multiple ownership. */
		return htable_add(&share_table, hash_vp_by_phys(share, NULL), share);
	}
}


/* add_share() in reverse. caller should release the physical page if the last
 * share was removed in which case rm_share() returns true, otherwise false.
 */
static bool rm_share(struct vp *vp)
{
	/* TODO: handle other forms of vp->status as they appear. */
	assert(~vp->status & 0x80000000);	/* in physical memory */
	size_t hash = hash_vp_by_phys(vp, NULL);
	struct pp *phys = get_pp(vp->status);
	struct vp *prev_owner = vp;
	if(!atomic_compare_exchange_strong(&phys->owner, &prev_owner, NULL)) {
		/* wasn't primary. */
		bool ok = htable_del(&share_table, hash, vp);
		assert(ok);
		if(prev_owner != NULL) return false;
	}
	return !has_shares(hash, vp->status, 1);
}


/* NOTE: this routine is all kinds of horseshit. instead, old <struct pl>
 * should be cleaned up with some kind of a garbage-collection step to get a
 * specified benefit for a linear crawl over every active physical page in the
 * system. iteration over page_active_list must deal with dead links already,
 * so it's not a big deal interfacewise.
 *
 * in fact, garbage collection has a name: page replacement. there should
 * probably be some kind of an accumulated vs. collected garbage counter, or
 * pair thereof, to indicate when garbage collection should be run (or some
 * extra turns of page replacement, since those'll touch the structures
 * anyway).
 *
 * also, darrays, how cute. it's application shitcode in a kernel component,
 * introducing malloc's locking to otherwise lockfree code.
 */
static void remove_active_pls(struct pl **pls, int n_pls)
{
	assert(e_inside());
	if(n_pls == 0) return;
	if(n_pls > 1) qsort(pls, n_pls, sizeof *pls, &cmp_ptrs);
	struct nbsl_iter it;
	for(struct nbsl_node *n = nbsl_first(&page_active_list, &it);
		n != NULL;
		n = nbsl_next(&page_active_list, &it))
	{
		struct pl *cur = container_of(n, struct pl, nn),
			**got = n_pls > 1 ? bsearch(&cur, pls, n_pls, sizeof *pls, &cmp_ptrs)
				: (cur == *pls ? pls : NULL);
		if(got == NULL) continue;
		assert(*got == cur);
		bool ok = nbsl_del_at(&page_active_list, &it);
		if(!ok) {
			printf("vm:%s: concurrent nbsl_del_at()?\n", __func__);
			/* FIXME: consider what to do here. it shouldn't happen in the
			 * single-threaded vm yet.
			 */
			assert(false);
		}
		e_free(cur);
	}
}


/* puts the <struct pp> referenced by @link0 into the free page list, and adds
 * @link0 to @plbuf.
 */
static void free_page(struct pl *link0, plbuf *plbuf)
{
	struct pp *phys = pl2pp(link0);
	atomic_store_explicit(&link0->status, 0, memory_order_release);
	push_page(&page_free_list, link0);
	assert(atomic_load(&phys->link) != link0);
	darray_push(*plbuf, link0);
}


/* adds dropped active_page_list links to @plbuf. */
static void remove_vp(struct vp *vp, plbuf *plbuf)
{
	assert(e_inside());
	assert(~vp->status & 0x80000000);	/* must be resident */
	struct pp *phys = get_pp(vp->status);
	struct pl *link0 = atomic_load_explicit(&phys->link,
		memory_order_acquire);
	if(PL_IS_PRIVATE(link0) && !VP_IS_COW(vp)) {
		assert(atomic_load(&phys->owner) == vp);	/* single owner */
		assert(!has_shares(int_hash(link0->page_num), link0->page_num, 1));
		struct pl *link = atomic_exchange(&phys->link, NULL);
		assert(link0 == link);	/* private, so won't have changed */
		struct vp *old = atomic_exchange(&phys->owner, NULL);
		assert(old == vp);	/* likewise */
		free_page(link0, plbuf);
	} else if(rm_share(vp) && VP_IS_ANON(vp)) {
		/* eagerly release pages that're not in the page cache. */
		free_page(link0, plbuf);
	}
	assert(atomic_load(&phys->owner) != vp);
	e_free(vp);
}


static int vm_munmap(L4_Word_t addr, uint32_t size)
{
	int sender_pid = pidof_NP(muidl_get_sender());
	if(sender_pid == 0 || sender_pid > SNEKS_MAX_PID) return -EINVAL;
	if(!VALID_ADDR_SIZE(addr, size)) return -EINVAL;
	struct vm_space *sp = ra_id2ptr(vm_space_ra, sender_pid);
	if(sp->kip_area.raw == 0) return -EINVAL;

	int eck = e_begin();
	munmap_space(sp, addr, size);
	e_end(eck);

	return 0;
}


static int fork_maps(struct vm_space *src, struct vm_space *dest)
{
	RB_FOREACH(cur, &src->maps) {
		struct lazy_mmap *orig = container_of(cur, struct lazy_mmap, rb),
			*copy = malloc(sizeof *copy);
		if(copy == NULL) goto Enomem;
		*copy = *orig;
		if(IS_ANON_MMAP(copy)) {
			assert(copy->fd_serv.raw == L4_MyGlobalId().raw);
			bool ok = htable_add(&anon_mmap_table,
				hash_lazy_mmap_by_ino(copy, NULL), copy);
			if(!ok) goto Enomem;
		}
		/* FIXME: duplicate file descriptors etc. */
		void *dupe = insert_lazy_mmap(dest, copy);
		assert(dupe == NULL);
	}
	return 0;

Enomem:
	/* FIXME: cleanup! */
	return -ENOMEM;
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
	struct vp *copy = NULL;
	L4_Fpage_t unmaps[64];
	int unmap_pos = 0, n_pages = 0;
	struct htable_iter it;
	for(struct vp *cur = htable_first(&src->pages, &it);
		cur != NULL;
		cur = htable_next(&src->pages, &it))
	{
		assert(VP_IS_COW(cur) ^ !!(VP_RIGHTS(cur) & L4_Writable));

		if(copy == NULL) {
			copy = malloc(sizeof *copy);
			if(copy == NULL) goto Enomem;
		}
		size_t hash = hash_vp_by_vaddr(cur, NULL);

/* (use this after lazy brk has been removed, subsequent to pf rejigger.) */
#if 0
		/* NOTE: for now, all pages are in memory. at some point swapped
		 * pages and pages removed from page cache may appear.
		 */
		assert(cur->status & ~0x80000000);
#endif

		if(!VP_IS_ANON(cur)
			&& (VP_IS_SHARED(cur) || (~VP_RIGHTS(cur) & L4_Writable)))
		{
			/* ignore private+file+ro or shared pages since they'll be mapped
			 * lazily using the page cache.
			 */
#ifdef TRACE_FORK
			printf("vm: full pagecache page ignored at vaddr=%#x\n",
				cur->vaddr & ~PAGE_MASK);
#endif
			continue;
		} else if(VP_IS_SHARED(cur)) {
			/* shared pages get another reference and the show goes on.
			 *
			 * FIXME: this appears to be a dead case, since all pages shared
			 * but not under copy-on-write are visible through the page cache.
			 */
#ifdef TRACE_FORK
			printf("vm: forking shared page at vaddr=%#x\n",
				cur->vaddr & ~PAGE_MASK);
#endif
			abort();	/* retain until hit */
			*copy = (struct vp){
				.vaddr = cur->vaddr,
				.status = cur->status,
				.age = 1,
			};
			bool ok = htable_add(&dest->pages, hash, copy);
			if(!ok || !add_share(cur->status, copy)) goto Enomem;
		} else {
			/* anonymous and private pages get copy-on-write. this applies
			 * even if said pages were read-only right now and made writable
			 * later.
			 *
			 * TODO: test this once mprotect() appears.
			 */
#ifdef TRACE_FORK
			printf("vm: cow for anon/private page at vaddr=%#x\n",
				cur->vaddr & ~PAGE_MASK);
#endif
			/* older TODO: indicate read-only sharing somewhere besides
			 * share_table.
			 */
			bool unmap = !VP_IS_COW(cur) && (VP_RIGHTS(cur) & L4_Writable) != 0;
			*copy = (struct vp){
				.vaddr = (cur->vaddr & ~L4_Writable)
					| ((VP_RIGHTS(cur) & L4_Writable) ? VPF_COW : 0),
				.status = cur->status,
				.age = 1,
			};
			bool ok = htable_add(&dest->pages, hash, copy);
			if(!ok) goto Enomem;
			if(unmap) {
				cur->vaddr |= VPF_COW;
				cur->vaddr &= ~L4_Writable;
				assert(VP_IS_COW(cur));
			}
			if(!add_share(cur->status, copy)) {
				if(unmap) {
					cur->vaddr &= ~VPF_COW;
					cur->vaddr |= L4_Writable;
					assert(!VP_IS_COW(cur));
				}
				goto Enomem;
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

		n_pages++; copy = NULL;
	}
	free(copy);

end:
	if(unmap_pos > 0) L4_UnmapFpages(unmap_pos, unmaps);
	return n_pages;

Enomem:
	n_pages = -ENOMEM;
	goto end;
}


static int vm_fork(uint16_t srcpid, uint16_t destpid)
{
	assert(invariants());
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
	dest->maps = RB_ROOT;
	dest->as_free = RB_ROOT;
	if(src == NULL) {
		/* creates an unconfigured address space as for spawn. caller should
		 * use VM::configure() to set it up before causing page faults.
		 */
		dest->kip_area.raw = ~0ul;
		dest->utcb_area = L4_Nilpage;
		dest->brk_lo = 0; dest->brk_hi = 0;
		dest->mmap_bot = user_addr_max;
		memset(dest->brk_bloom, 0, sizeof dest->brk_bloom);
	} else {
		/* fork from @src. */
		dest->kip_area = src->kip_area;
		dest->utcb_area = src->utcb_area;
		dest->sysinfo_area = src->sysinfo_area;
		dest->brk_lo = src->brk_lo;
		dest->brk_hi = src->brk_hi;
		dest->mmap_bot = src->mmap_bot;
		int n = fork_maps(src, dest);
		if(n == 0) n = fork_pages(src, dest);
		if(n < 0) {
			/* FIXME: cleanup */
			return n;
		}
		memcpy(dest->brk_bloom, src->brk_bloom, sizeof dest->brk_bloom);
	}

	assert(invariants());
	return ra_ptr2id(vm_space_ra, dest);
}


static int vm_configure(
	L4_Word_t *last_resv_p,
	uint16_t pid, L4_Fpage_t utcb, L4_Fpage_t kip)
{
	assert(invariants());
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

	assert(invariants());
	*last_resv_p = L4_Address(sp->sysinfo_area) + L4_Size(sp->sysinfo_area) - 1;
	return 0;
}


static int vm_upload_page(
	uint16_t target_pid, L4_Word_t addr,
	const uint8_t *data, unsigned data_len,
	int prot, int flags)
{
	assert(invariants());
	struct vm_space *sp = ra_id2ptr(vm_space_ra, target_pid);
	if(unlikely(L4_IsNilFpage(sp->utcb_area))) return -EINVAL;
	if((addr & PAGE_MASK) != 0) return -EINVAL;
	if(~flags & MAP_ANONYMOUS) return -EINVAL;	/* can't specify file */
	if(~flags & MAP_FIXED) return -EINVAL;		/* per spec */
	if(prot == 0) return -EINVAL;
	if(flags & MAP_SHARED) return -ENOSYS;	/* FIXME when required */
	if(~flags & MAP_PRIVATE) return -EINVAL;/* ^- wew lad */

	struct vp *v = malloc(sizeof *v);
	if(unlikely(v == NULL)) return -ENOMEM;
	v->vaddr = addr | prot_to_l4_rights(prot) | VPF_ANON;
	v->age = 1;
	assert(VP_RIGHTS(v) == prot_to_l4_rights(prot));

	/* insert lazy_mmap to maintain "mmap or brk" invariants.
	 * (i'm not sure if that's useful.)
	 */
	struct lazy_mmap *mm = malloc(sizeof *mm);
	if(mm == NULL) {
		free(v);
		return -ENOMEM;
	}
	*mm = (struct lazy_mmap){
		.flags = VP_RIGHTS(v) << 16 | (flags & ~MAP_FIXED),
	};
	int n = reserve_mmap(mm, sp, addr, PAGE_SIZE, true);
	if(n < 0) {
		free(v);
		free(mm);
		return n;
	}
	assert(mm->addr == addr && mm->length == PAGE_SIZE);
	assert(~mm->flags & MAP_FIXED);

	size_t hash = int_hash(addr);
	struct vp *old = htable_get(&sp->pages, hash, &cmp_vp_to_vaddr, &addr);
	if(old != NULL) {
		htable_del(&sp->pages, hash, old);
		plbuf pls = darray_new();
		remove_vp(old, &pls);
		flush_plbuf(&pls);
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
	atomic_store_explicit(&get_pp(v->status)->owner, v, memory_order_relaxed);
	e_end(eck);

	/* plunk it in there. */
	assert(hash_vp_by_vaddr(v, NULL) == hash);
	bool ok = htable_add(&sp->pages, hash, v);
	if(unlikely(!ok)) {
		/* FIXME: roll back and return an error. */
		printf("%s: can't handle failing htable_add()!\n", __func__);
		abort();
	}

	assert(invariants());
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
	assert(invariants());

	/* userspace tasks can't have I/O ports. at all. it is verboten. */
	printf("%s: IO fault from pid=%d, ip=%#lx, port=%#lx:%#lx\n",
		__func__, pidof_NP(muidl_get_sender()), fip,
		L4_IoFpagePort(fault), L4_IoFpageSize(fault));
	muidl_raise_no_reply();
}


static L4_Fpage_t pf_cow(struct vp *virt)
{
	assert(VP_IS_COW(virt));
	assert(~VP_RIGHTS(virt) & L4_Writable);

	struct pp *phys = get_pp(virt->status);
	struct vp *primary = atomic_load_explicit(&phys->owner,
		memory_order_relaxed);
	size_t hash = int_hash(virt->status);
	if(primary != virt) {
		/* our share was secondary; remove it. */
		bool ok = htable_del(&share_table, hash, virt);
		if(!ok) {
			printf("%s: expected share, wasn't found???\n", __func__);
			abort();
		}
	}
	/* is @virt the sole owner of the page, which isn't in the page cache? */
	if(VP_IS_ANON(virt)
		&& ((primary == virt && !has_shares(hash, virt->status, 1))
			|| (primary == NULL && !has_shares(hash, virt->status, 2))))
	{
		/* it is. take ownership. */
		assert(primary == NULL || atomic_load(&phys->owner) == virt);
		if(primary == NULL) {
			/* take primary share. */
			atomic_store_explicit(&phys->owner, virt, memory_order_relaxed);
		}
		virt->vaddr &= ~VPF_COW;
		virt->vaddr |= L4_Writable;
		/* (shared anonymous memory is never COW.) */
		assert(~virt->vaddr & VPF_SHARED);
	} else {
		/* no. make a copy. */
		if(primary == virt) {
			/* drop primary share. */
			atomic_store_explicit(&phys->owner, NULL, memory_order_relaxed);
		}
		struct pl *newpl = get_free_pl();
		memcpy((void *)((uintptr_t)newpl->page_num << PAGE_BITS),
			(void *)((uintptr_t)virt->status << PAGE_BITS),
			PAGE_SIZE);
		virt->status = newpl->page_num;
		virt->vaddr &= ~(VPF_COW | VPF_SHARED);
		virt->vaddr |= L4_Writable | VPF_ANON;
		atomic_store_explicit(&pl2pp(newpl)->owner, virt,
			memory_order_release);
		push_page(&page_active_list, newpl);
		e_free(newpl);
	}

	L4_Fpage_t map_page = L4_FpageLog2(virt->status << PAGE_BITS, PAGE_BITS);
	L4_Set_Rights(&map_page, VP_RIGHTS(virt));
	return map_page;
}


/* finds the cached page, or reads it from @mm if it's not in cache.
 * @bump is # of pages from @mm start. @vp is the virtual page that'll take
 * ownership of a freshly-loaded page. return value is negative errno, or 0
 * when found, or 1 when loaded. *@cached_p will be filled in.
 *
 * TODO: in the future this function will do some sort of continuation
 * business to allow vm to respond while waiting for IO. for now, not so much.
 * this'll also require some way to wait on ongoing fetches, so the actual
 * item that lands in the page cache may also be a placeholder.
 */
static int fetch_cached_page(
	struct pl **cached_p,
	const struct lazy_mmap *mm, int bump, struct vp *vp)
{
	assert(e_inside());
	struct nbsl_node *top;
	struct pl *cached = find_cached_page(&top, mm, bump);
	if(cached != NULL) {
		/* hit! */
		TRACE_FAULT("vm:%s:pc hit on %x:%lx:%x\n", __func__,
			pidof_NP(mm->fd_serv), (unsigned long)mm->ino,
			mm->offset + bump);
		*cached_p = cached;
		return 0;
	} else {
		/* allocate, read, and try to insert. */
		struct pl *link = get_free_pl();
		void *page = (void *)((uintptr_t)link->page_num << PAGE_BITS);
		unsigned n_bytes = 0;
		if(~mm->flags & MAP_ANONYMOUS) {
			n_bytes = PAGE_SIZE;
			int n = __fs_read(mm->fd_serv, page, &n_bytes, mm->ino,
				(mm->offset + bump) * PAGE_SIZE, PAGE_SIZE);
			if(unlikely(n != 0)) {
				push_page(&page_free_list, link);
				e_free(link);
				return n > 0 ? -EIO : n;
			}
			if(n_bytes < PAGE_SIZE) {
				TRACE_FAULT("vm:%s: short read (got %u bytes)\n", __func__, n_bytes);
			}
		}
		if(n_bytes < PAGE_SIZE) {
			memset(page + n_bytes, '\0', PAGE_SIZE - n_bytes);
		}

		/* insert. (it's ok to modify link->foo since those fields are unused
		 * in the freelist. push_cached_page() always adds a new link.)
		 */
		link->fsid_ino = (uint64_t)pidof_NP(mm->fd_serv) << 48
			| (mm->ino & ~(0xffffull << 48));
		assert(PL_FSID(link) == pidof_NP(mm->fd_serv));
		assert(PL_INO(link) == mm->ino);
		link->offset = mm->offset + bump;
		atomic_store(&pl2pp(link)->owner, vp);
		int n = push_cached_page(&cached, top, link);
		if(n == 0) {
			assert(cached != NULL);
			TRACE_FAULT("vm:%s:pc miss on %x:%lx:%x, vp'=%p, phys=%p\n", __func__,
				pidof_NP(mm->fd_serv), (unsigned long)mm->ino,
				mm->offset + bump, vp, pl2pp(link));
			n = 1;	/* actual miss and insert */
			assert(pl2pp(cached)->owner == vp);
		} else {
			assert(n == -EEXIST);
			push_page(&page_free_list, link);
			assert(cached != NULL);
			TRACE_FAULT("vm:%s:pc false miss on %x:%lx:%x\n", __func__,
				pidof_NP(mm->fd_serv), (unsigned long)mm->ino,
				mm->offset + bump);
			n = 0;	/* it's a hit after all */
		}
		e_free(link);

		*cached_p = cached;
		return n;
	}
}


/* handling of faults on maps with MAP_SHARED, or file-backed but not a write
 * fault (so a caching candidate).
 */
static int pf_mmap_shared(
	L4_Fpage_t *map_page_p, struct vp *vp,	/* out */
	const struct lazy_mmap *mm, L4_Word_t faddr, int fault_rwx)
{
	assert(e_inside());

	struct nbsl_node *top;
	int bump = ((faddr & ~PAGE_MASK) - mm->addr) >> PAGE_BITS;
	struct pl *cached = find_cached_page(&top, mm, bump);
	if(cached == NULL) {
		/* allocate. */
		struct pl *link = get_free_pl();
		void *page = (void *)((uintptr_t)link->page_num << PAGE_BITS);
		unsigned n_bytes = 0;
		if(~mm->flags & MAP_ANONYMOUS) {
			n_bytes = PAGE_SIZE;
			int n = __fs_read(mm->fd_serv, page, &n_bytes, mm->ino,
				mm->offset * PAGE_SIZE + ((faddr & ~PAGE_MASK) - mm->addr),
				PAGE_SIZE);
			if(unlikely(n != 0)) {
				push_page(&page_free_list, link);
				e_free(link);
				return n;
			}
#ifdef TRACE_FAULTS
			if(n_bytes < PAGE_SIZE) {
				TRACE_FAULT("vm:%s: short read (got %u bytes)\n", __func__, n_bytes);
			}
#endif
		}
		if(n_bytes < PAGE_SIZE) {
			memset(page + n_bytes, '\0', PAGE_SIZE - n_bytes);
		}

		/* insert. (it's ok to modify link->foo since those fields are unused
		 * in the freelist. push_cached_page() always adds a new link.)
		 */
		link->fsid_ino = (uint64_t)pidof_NP(mm->fd_serv) << 48
			| (mm->ino & ~(0xffffull << 48));
		assert(PL_FSID(link) == pidof_NP(mm->fd_serv));
		assert(PL_INO(link) == mm->ino);
		link->offset = mm->offset + bump;
		int n = push_cached_page(&cached, top, link);
		if(n == 0) {
			*map_page_p = L4_FpageLog2((L4_Word_t)page, PAGE_BITS);
			TRACE_FAULT("vm:%s:pc miss on %x:%lx:%x, vp'=%p, phys=%p\n", __func__,
				pidof_NP(mm->fd_serv), (unsigned long)mm->ino,
				mm->offset + bump, vp, pl2pp(link));
		} else {
			assert(n == -EEXIST);
			push_page(&page_free_list, link);
			assert(cached != NULL);
			TRACE_FAULT("vm:%s:pc false miss on %x:%lx:%x\n", __func__,
				pidof_NP(mm->fd_serv), (unsigned long)mm->ino,
				mm->offset + bump);
		}
		e_free(link);
	}
	if(cached != NULL) {
		/* share. possibly from an abortive cache insert. */
		/* FIXME: overwritten in caller, required by hash_vp_by_phys() i.e.
		 * add_share().
		 */
		vp->status = cached->page_num;
		int n = add_share(cached->page_num, vp);
		if(n < 0) return n;
		*map_page_p = L4_FpageLog2(cached->page_num << PAGE_BITS, PAGE_BITS);
		TRACE_FAULT("vm:%s:pc hit on %x:%lx:%x\n", __func__,
			pidof_NP(mm->fd_serv), (unsigned long)mm->ino,
			mm->offset + bump);
	}

	vp->vaddr |= VPF_SHARED;
	return 0;
}


/* handling MAP_PRIVATE non-anonymous write faults, i.e. those that
 * pf_mmap_shared() doesn't. the mechanical difference is that this one makes
 * a copy of the file-backed page if it's found in the page cache, possibly
 * inserting it first if not already found.
 */
static int pf_mmap_private(
	L4_Fpage_t *map_page_p, struct vp *vp,	/* out */
	const struct lazy_mmap *mm, L4_Word_t faddr, int fault_rwx)
{
	assert(e_inside());
	assert(fault_rwx & L4_Writable);

	struct pl *cached;
	int offset = ((faddr & ~PAGE_MASK) - mm->addr) >> PAGE_BITS,
		n = fetch_cached_page(&cached, mm, offset, vp);
	if(n < 0) return n;
	assert(cached != NULL);

	struct pl *link = get_free_pl();
	*map_page_p = L4_FpageLog2((uintptr_t)link->page_num << PAGE_BITS,
		PAGE_BITS);
	memcpy((void *)L4_Address(*map_page_p),
		(void *)((uintptr_t)cached->page_num << PAGE_BITS), PAGE_SIZE);
	atomic_store_explicit(&pl2pp(link)->owner, vp, memory_order_relaxed);
	push_page(&page_active_list, link);
	e_free(link);
	vp->vaddr |= VPF_ANON;
	assert(!VP_IS_SHARED(vp));
	assert(!VP_IS_COW(vp));

	if(n == 1) {
		/* drop ownership of the freshly-loaded page. */
		struct vp *old = atomic_exchange(&pl2pp(cached)->owner, NULL);
		assert(old == vp);
	}

	return 0;
}


static void get_brk_bloom_key(int k[static 3], size_t hash)
{
	const int bits = size_to_shift(BRK_BLOOM_WORDS * sizeof(L4_Word_t) * 8),
		mask = (1 << bits) - 1;
	assert(bits <= sizeof(L4_Word_t) * 8 / 3);
	for(int i=0; i < 3; i++, hash >>= bits) k[i] = hash & mask;
	/* TODO: cook into distinct values */
}


/* brk fastpath. always generates private anonymous memory. returns -1 when
 * @faddr_page isn't in the brk range, 1 when it is and an old page should be
 * remapped, and 0 when a new page was allocated and *@map_page filled in.
 *
 * TODO: merge this in with the allocating vp path through vm_pf(), so that
 * malloc and htable_add, both with elaborate failure paths, aren't repeated.
 */
static int brk_fastpath(
	L4_Fpage_t *map_page, struct vp **old_p,
	struct vm_space *sp, L4_Word_t faddr_page, size_t hash)
{
	if(faddr_page < sp->brk_lo || faddr_page >= sp->brk_hi) return -1;

	const int word_bits = size_to_shift(sizeof(L4_Word_t) * 8),
		word_mask = (1u << word_bits) - 1;
	int k[3]; get_brk_bloom_key(k, hash);
	int hits = 0;
	for(int i=0; i < 3; i++) {
		int limb = k[i] >> word_bits;
		L4_Word_t bit = 1ul << (k[i] & word_mask);
		assert(limb < ARRAY_SIZE(sp->brk_bloom));
		hits += (sp->brk_bloom[limb] & bit) ? 1 : 0;
	}
	if(hits == 3) {
		/* might already be there. see if it is. */
		*old_p = htable_get(&sp->pages, hash, &cmp_vp_to_vaddr, &faddr_page);
		if(*old_p != NULL) return 1;
	}

	/* create new memory. */
	struct pl *link = get_free_pl();
	void *page = (void *)((uintptr_t)link->page_num << PAGE_BITS);
	memset(page, '\0', PAGE_SIZE);
	struct vp *vp = malloc(sizeof *vp);
	if(unlikely(vp == NULL)) {
		/* FIXME */
		abort();
	}
	*vp = (struct vp){
		.vaddr = faddr_page | L4_FullyAccessible | VPF_ANON,
		.status = link->page_num, .age = 1,
	};
	bool ok = htable_add(&sp->pages, hash, vp);
	if(unlikely(!ok)) {
		/* FIXME: roll back and segfault. */
		printf("%s: can't handle failing htable_add()!\n", __func__);
		abort();
	}
	atomic_store_explicit(&pl2pp(link)->owner, vp, memory_order_relaxed);
	push_page(&page_active_list, link);
	e_free(link);

	if(hits < 3) {
		/* add to the bloom filter. */
		for(int i=0; i < 3; i++) {
			sp->brk_bloom[k[i] >> word_bits] |= 1ul << (k[i] & word_mask);
		}
	}

	*map_page = L4_FpageLog2((L4_Word_t)page, PAGE_BITS);
	L4_Set_Rights(map_page, VP_RIGHTS(vp));
	return 0;
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

	const int fault_rwx = L4_Label(muidl_get_tag()) & 7;
	TRACE_FAULT("%s: pid=%d, faddr=%#lx, fip=%#lx, [%c%c%c]",
		__func__, pid, faddr, fip,
		(fault_rwx & L4_Readable) != 0 ? 'r' : '-',
		(fault_rwx & L4_Writable) != 0 ? 'w' : '-',
		(fault_rwx & L4_eXecutable) != 0 ? 'x' : '-');

	int eck = e_begin();
	assert(invariants());

	L4_Fpage_t map_page;
	L4_Word_t faddr_page = faddr & ~PAGE_MASK;
	size_t hash = int_hash(faddr_page);
	struct vp *old;
	n = brk_fastpath(&map_page, &old, sp, faddr_page, hash);
	if(n == 0) {
		TRACE_FAULT("  brk heap\n");
		goto reply;
	} else if(n < 0) {
		old = htable_get(&sp->pages, hash, &cmp_vp_to_vaddr, &faddr_page);
	} else {
		assert(old != NULL);
	}

	if(old != NULL && VP_IS_COW(old) && (fault_rwx & L4_Writable)) {
		TRACE_FAULT("  copy-on-write\n");
		map_page = pf_cow(old);
		goto reply;
	} else if(old != NULL && (VP_RIGHTS(old) & fault_rwx) != fault_rwx) {
		TRACE_FAULT("  no access!!!\n");
		printf("%s: segv (access=%#x, had=%#x)\n", __func__,
			fault_rwx, VP_RIGHTS(old));
		goto segv;
	} else if(old != NULL) {
		/* quick remap or expand. */
		TRACE_FAULT("  remap\n");
		map_page = L4_FpageLog2(old->status << PAGE_BITS, PAGE_BITS);
		L4_Set_Rights(&map_page, VP_RIGHTS(old));
		goto reply;
	} else if(unlikely(ADDR_IN_FPAGE(sp->sysinfo_area, faddr))) {
		TRACE_FAULT("  sysinfopage\n");
		assert(the_sip->magic == SNEKS_SYSINFO_MAGIC);
		map_page = L4_FpageLog2((uintptr_t)the_sip, PAGE_BITS);
		L4_Set_Rights(&map_page, L4_Readable);
		goto reply;
	}

	int rights;
	struct lazy_mmap *mm = find_lazy_mmap(sp, faddr);
	if(mm == NULL) {
		TRACE_FAULT("  segv (unmapped)\n");
		goto segv;
	} else {
		assert(faddr >= mm->addr && faddr < mm->addr + mm->length);
		rights = (mm->flags >> 16) & 7;	/* see comment at lazy_mmap decl */
		if(unlikely((fault_rwx & rights) != fault_rwx)) {
			TRACE_FAULT("  segv (access mode, mmap)\n");
			goto segv;
		}
	}

	struct vp *vp = malloc(sizeof *vp);
	if(unlikely(vp == NULL)) {
		/* FIXME */
		abort();
	}
	*vp = (struct vp){ .vaddr = faddr_page | rights, .age = 1 };

	if((mm->flags & MAP_SHARED)
		|| ((~fault_rwx & L4_Writable) && (~mm->flags & MAP_ANONYMOUS)))
	{
		TRACE_FAULT("  mmap/shared\n");
		/* look for it in the page cache, or load from a file. */
		n = pf_mmap_shared(&map_page, vp, mm, faddr, fault_rwx);
		if(n != 0) {
			printf("pf_mmap_shared failed! n=%d\n", n);
			abort();
			/* FIXME: clean up, detect EFAULT, etc., instead */
		}
		/* copy-on-write read faults on writable private+file maps */
		if(~fault_rwx & L4_Writable) {
			if(vp->vaddr & L4_Writable) {
				vp->vaddr |= VPF_COW;
				vp->vaddr &= ~L4_Writable;
			}
		}
		assert(VP_IS_SHARED(vp));
	} else if(mm->flags & MAP_ANONYMOUS) {
		TRACE_FAULT("  mmap/anon\n");
		struct pl *link;
		link = get_free_pl();
		map_page = L4_FpageLog2((uintptr_t)link->page_num << PAGE_BITS,
			PAGE_BITS);
		memset((void *)L4_Address(map_page), '\0', PAGE_SIZE);
		TRACE_FAULT("vm:%s: sets owner to vp=%p\n", __func__, vp);
		atomic_store_explicit(&pl2pp(link)->owner, vp, memory_order_relaxed);
		push_page(&page_active_list, link);
		e_free(link);
		vp->vaddr |= VPF_ANON;
	} else {
		TRACE_FAULT("  mmap/private\n");
		/* writes into private file maps; insert-or-lookup in page cache and
		 * make a copy.
		 */
		n = pf_mmap_private(&map_page, vp, mm, faddr, fault_rwx);
		if(n != 0) {
			printf("pf_mmap_private failed! n=%d\n", n);
			abort();
			/* FIXME: handle prettily instead */
		}
	}

	vp->status = L4_Address(map_page) >> PAGE_BITS;
	L4_Set_Rights(&map_page, VP_RIGHTS(vp));

	bool ok = htable_add(&sp->pages, hash_vp_by_vaddr(vp, NULL), vp);
	if(unlikely(!ok)) {
		/* FIXME: roll back and segfault. */
		printf("%s: can't handle failing htable_add()!\n", __func__);
		abort();
	}

reply:
	assert(invariants());
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
	assert(invariants());
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


static COLD void find_max_addr(void)
{
	L4_KernelInterfacePage_t *kip = L4_GetKernelInterface();
	struct memdescbuf md = {
		.ptr = L4_MemoryDesc(kip, 0),
		.len = kip->MemoryInfo.n,
	};
	L4_Word_t acc = 0;
	L4_Fpage_t fp;
	do {
		fp = mdb_query(&md, acc, ~0ul, true, false,
			L4_ConventionalMemoryType);
		if(!L4_IsNilFpage(fp) && L4_SizeLog2(fp) >= PAGE_BITS) {
			acc = max(acc, L4_Address(fp) + L4_Size(fp));
		}
	} while(!L4_IsNilFpage(fp));
	user_addr_max = acc - 1;
}


int main(int argc, char *argv[])
{
	printf("vm sez hello!\n");
	anon_fsid = pidof_NP(L4_MyGlobalId());
	L4_ThreadId_t init_tid;
	int n_phys = 0;
	L4_Fpage_t *phys = init_protocol(&n_phys, &init_tid);
	printf("vm: init protocol done.\n");

	int eck = e_begin();
	assert(invariants());
	init_phys(phys, n_phys);
	assert(invariants());
	init_pc(phys, n_phys);
	assert(invariants());
	free(phys);
	e_end(eck);

	/* the rest of the owl */
	find_max_addr();
	vm_space_ra = RA_NEW(struct vm_space, SNEKS_MAX_PID + 1);
	get_services();
	assert(invariants());

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
		.munmap = &vm_munmap,
		.fork = &vm_fork,
		.configure = &vm_configure,
		.upload_page = &vm_upload_page,
		.breath_of_life = &vm_breath_of_life,
		.brk = &vm_brk,
		.erase = &vm_erase,

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
			assert(invariants());
		}
	}

	return 0;
}
