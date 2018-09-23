
/* userspace API portion of the root task. */

#define ROOTUAPI_IMPL_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <sys/mman.h>
#include <ccan/htable/htable.h>
#include <ccan/darray/darray.h>
#include <ccan/bitmap/bitmap.h>
#include <ccan/minmax/minmax.h>
#include <ccan/array_size/array_size.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <ukernel/rangealloc.h>
#include <sneks/mm.h>
#include <sneks/process.h>

#include "muidl.h"
#include "vm-defs.h"
#include "fs-defs.h"
#include "info-defs.h"
#include "proc-defs.h"
#include "root-uapi-defs.h"
#include "elf.h"
#include "defs.h"


#define MAX_PID 0xffff
#define IS_SYSTASK(pid) ((pid) >= SNEKS_MIN_SYSID)

#define TNOS_PER_BITMAP (1 << 16)


typedef darray(L4_ThreadId_t) tidlist;


/* common portion for the L4.X2 address space configuration and thread
 * management.
 */
struct task_common {
	L4_Fpage_t kip_area, utcb_area;
	tidlist threads;
	bitmap *utcb_free;
};


/* userspace processes. allocated in ra_process. */
struct process
{
	struct task_common task;
	/* TODO: add bits for wait(2) lists, parent PID, uid, gid, etc. */
};


/* system tasks. allocated in ra_systask. note that ra_systask's id=0 is
 * available and maps to SNEKS_MIN_SYSID + 0.
 */
struct systask
{
	struct task_common task;
	/* (systask scheduling things would go here eventually) */
};


static struct rangealloc *ra_process = NULL, *ra_systask = NULL;
static int utcb_size_log2;
L4_ThreadId_t uapi_tid;

/* TID allocator. this is for the 32-bit mode where the two highest bits of
 * ThreadNo are the two highest bits of the process ID.
 */
static bitmap *tno_free_maps[4];
static int tno_free_counts[4];

/* PID allocator, backed by rangealloc. */
static _Atomic int next_user_pid = 1;


void lock_uapi(void)
{
	assert(!L4_SameThreads(uapi_tid, L4_Myself()));
	/* TODO: try exchangeregisters to halt uapi_tid in ipc wait, or do ipc
	 * with uapi_tid and repeat.
	 */
}


void unlock_uapi(void)
{
	assert(!L4_SameThreads(uapi_tid, L4_Myself()));
	/* resume uapi_tid. */
}


static void free_threadno(L4_ThreadId_t tid)
{
	int map = L4_ThreadNo(tid) >> 16, bit = L4_ThreadNo(tid) & 0xffff;
	bitmap_set_bit(tno_free_maps[map], bit);
	tno_free_counts[map]++;
	assert(tno_free_counts[map] <= TNOS_PER_BITMAP);
}


static struct process *get_process(int pid)
{
	if(pid <= 0 || pid > SNEKS_MAX_PID) return NULL;
	struct process *p = ra_id2ptr_safe(ra_process, pid);
	if(p == NULL || L4_IsNilFpage(p->task.utcb_area)) return NULL;
	return p;
}


static struct systask *get_systask(int pid)
{
	if(pid < SNEKS_MIN_SYSID || pid > 65535) return NULL;
	struct systask *st = ra_id2ptr_safe(ra_systask, pid - SNEKS_MIN_SYSID);
	if(st == NULL || L4_IsNilFpage(st->task.utcb_area)) return NULL;
	return st;
}


static struct task_common *get_task(int pid)
{
	if(IS_SYSTASK(pid)) {
		struct systask *st = get_systask(pid);
		if(st != NULL) return &st->task;
	} else {
		struct process *p = get_process(pid);
		if(p != NULL) return &p->task;
	}
	return NULL;
}


static bool task_common_ctor(
	struct task_common *task,
	L4_Fpage_t kip, L4_Fpage_t utcb)
{
	task->utcb_area = L4_Nilpage;
	task->utcb_free = bitmap_alloc1(
		1 << (L4_SizeLog2(utcb) - utcb_size_log2));
	if(task->utcb_free == NULL) return false;
	task->kip_area = kip;
	task->utcb_area = utcb;
	darray_init(task->threads);
	return true;
}


/* destroys threads, releases tno bitmap slots, clears the tidlist, tosses
 * associated memory, and invalidates the structure.
 */
static void task_common_dtor(struct task_common *task)
{
	for(int i=0; i < task->threads.size; i++) {
		L4_ThreadId_t tid = task->threads.item[i];
		L4_Word_t res = L4_ThreadControl(tid, L4_nilthread, L4_nilthread,
			L4_nilthread, (void *)-1);
		if(res != 1) {
			printf("%s: ThreadControl failed, ec=%lu\n", __func__,
				L4_ErrorCode());
			abort();	/* fix whatever caused this */
		}
		free_threadno(tid);
	}
	darray_free(task->threads);
	free(task->utcb_free);
	task->utcb_area = L4_Nilpage;
}


int add_systask(int pid, L4_Fpage_t kip, L4_Fpage_t utcb)
{
	if(pid >= 0 && pid < SNEKS_MIN_SYSID) return -EINVAL;

	struct systask *st = ra_alloc(ra_systask,
		pid < 0 ? -1 : pid - SNEKS_MIN_SYSID);
	if(st == NULL) return pid < 0 ? -ENOMEM : -EEXIST;

	if(!task_common_ctor(&st->task, kip, utcb)) {
		ra_free(ra_systask, st);
		return -ENOMEM;
	}

	if(pid == SNEKS_MIN_SYSID) {
		static bool done = false;
		assert(!done);
		done = true;

		/* roottask initialization. */
		darray_push(st->task.threads, L4_MyGlobalId());
		bitmap_clear_bit(st->task.utcb_free, 0);
	} else if(pid == pidof_NP(L4_Pager())) {
		static bool done = false;
		assert(!done);
		done = true;

		/* sysmem init. */
		darray_push(st->task.threads, L4_Pager());
		bitmap_clear_bit(st->task.utcb_free, 0);
	}

	return ra_ptr2id(ra_systask, st) + SNEKS_MIN_SYSID;
}


L4_ThreadId_t allocate_thread(int pid, void **utcb_loc_p)
{
	assert(utcb_loc_p != NULL);

	int map = IS_SYSTASK(pid) ? 0 : (pid & 0x6000) >> 13;
	while(tno_free_counts[map] == 0 && IS_SYSTASK(pid) && map < 4) map++;
	if(map > 3 || tno_free_counts[map] == 0) {
		printf("uapi: no thread#s left!\n");
		return L4_nilthread;
	}

	struct task_common *task = get_task(pid);
	if(task == NULL) return L4_nilthread;

	/* TODO: get better bounds for both calls to bitmap_ffs(). */
	int bit = bitmap_ffs(tno_free_maps[map], 0, TNOS_PER_BITMAP);
	assert(bit < TNOS_PER_BITMAP);
	bitmap_clear_bit(tno_free_maps[map], bit);

	L4_ThreadId_t tid = L4_GlobalId(map << 16 | bit,
		IS_SYSTASK(pid) ? (pid - SNEKS_MIN_SYSID) << 2 | 2
			: (pid & 0x1fff) << 1 | 1);
#if 0
	printf("%s: map=%d, bit=%d ==> tid=%#lx (%lu:%lu)\n", __func__, map, bit,
		tid.raw, L4_ThreadNo(tid), L4_Version(tid));
	printf("%s: pid=%d, pidof_NP(%#lx)=%d\n", __func__,
		pid, tid.raw, pidof_NP(tid));
#endif
	assert(pidof_NP(tid) == pid);

	int n_slots = 1 << (L4_SizeLog2(task->utcb_area) - utcb_size_log2),
		utcb_slot = bitmap_ffs(task->utcb_free, 0, n_slots);
	if(utcb_slot == n_slots) {
#if 0
		printf("%s: no more utcb slots (area=%#lx:%#lx)\n", __func__,
			L4_Address(ta->base.utcb_area), L4_Size(ta->base.utcb_area));
#endif
		bitmap_set_bit(tno_free_maps[map], bit);
		return L4_nilthread;
	}
	bitmap_clear_bit(task->utcb_free, utcb_slot);
	darray_push(task->threads, tid);

	*utcb_loc_p = (void *)(L4_Address(task->utcb_area)
		+ (utcb_slot << utcb_size_log2));
	return tid;
}


static void tid_remove_fast(tidlist *tids, int ix)
{
	if(ix < tids->size - 1) tids->item[ix] = tids->item[tids->size - 1];
	if(--tids->size < tids->alloc / 2) {
		darray_realloc(*tids, tids->alloc / 2);
	}
}


static bool tidlist_remove_from(tidlist *tids, L4_ThreadId_t tid)
{
	tid = L4_GlobalIdOf(tid);
	int found = -1;
	for(size_t i=0; i < tids->size; i++) {
		if(tids->item[i].raw == tid.raw) {
			found = i;
			break;
		}
	}
	if(found < 0) return false;
	else {
		tid_remove_fast(tids, found);
		return true;
	}
}


/* for rolling back allocate_thread() when make_space() fails. */
static void free_thread(
	struct task_common *task,
	L4_ThreadId_t tid, void *utcb_loc)
{
	free_threadno(tid);
	L4_Word_t pos = (L4_Word_t)utcb_loc - L4_Address(task->utcb_area),
		slot = pos >> utcb_size_log2;
	assert((L4_Word_t)utcb_loc >= L4_Address(task->utcb_area)
		&& pos < L4_Size(task->utcb_area));
	assert(!bitmap_test_bit(task->utcb_free, slot));
	bitmap_set_bit(task->utcb_free, slot);
	tidlist_remove_from(&task->threads, tid);
}


static void destroy_process(struct process *p)
{
	task_common_dtor(&p->task);
	ra_free(ra_process, p);
}


static int make_space(
	L4_ThreadId_t tid, L4_Fpage_t kip_area, L4_Fpage_t utcb_area)
{
	L4_Word_t res = L4_ThreadControl(tid, tid, L4_Myself(), L4_Myself(),
		(void *)-1);
	if(res != 1) {
		printf("%s: ThreadControl failed, ec=%lu\n", __func__, L4_ErrorCode());
		abort();	/* FIXME: instead, translate error code */
	}

	L4_Word_t old_ctl;
	res = L4_SpaceControl(tid, 0, kip_area, utcb_area, L4_anythread, &old_ctl);
	if(res != 1) {
		printf("%s: SpaceControl failed, ec=%lu\n", __func__, L4_ErrorCode());
		abort();	/* FIXME: instead, translate error code */
	}

	return 0;
}


static int uapi_create_thread(L4_Word_t *tid_ptr)
{
	L4_ThreadId_t sender = muidl_get_sender();
	int pid = pidof_NP(sender);
	if(pid == 0) return -EINVAL;

	struct task_common *ta = get_task(pid);
	if(ta == NULL) return -EINVAL;

	bool first_thread = ta->threads.size == 0;
	void *utcb_loc;
	L4_ThreadId_t tid = allocate_thread(pid, &utcb_loc);
	if(L4_IsNilThread(tid)) {
		printf("%s: can't allocate thread ID\n", __func__);
		return -ENOMEM;
	}
	assert(L4_ThreadNo(tid) >= 500);
	if(first_thread) {
		/* lazy-init a new address space. */
		printf("make_space(%lu:%lu, ...)\n", L4_ThreadNo(tid), L4_Version(tid));
		int n = make_space(tid, ta->kip_area, ta->utcb_area);
		if(n < 0) {
			free_thread(ta, tid, utcb_loc);
			return n;
		}
	}

	/* TODO: use vm_tid for non-systask paging. */
	L4_ThreadId_t space = ta->threads.item[0],
		pager = IS_SYSTASK(pid) ? L4_Pager() : L4_nilthread;
	L4_Word_t res = L4_ThreadControl(tid, space, L4_Myself(), pager, utcb_loc);
	if(res != 1) {
		printf("%s: ThreadControl failed, ec=%lu\n", __func__, L4_ErrorCode());
		printf("%s: ... tid=%lu:%lu, space=%lu:%lu\n", __func__,
			L4_ThreadNo(tid), L4_Version(tid),
			L4_ThreadNo(space), L4_Version(space));
		abort();	/* FIXME: translate! */
	}
	*tid_ptr = tid.raw;

	return 0;
}


static int uapi_remove_thread(L4_Word_t raw_tid, L4_Word_t utcb_addr)
{
	L4_ThreadId_t tid = { .raw = raw_tid };
	if(L4_IsLocalId(tid)) return -EINVAL;

	L4_ThreadId_t sender = muidl_get_sender();
	bool is_self = L4_SameThreads(tid, sender);
	int pid = pidof_NP(L4_GlobalIdOf(sender));
	if(pid <= 0) return -EINVAL;
	struct task_common *ta = get_task(pid);
	if(ta == NULL) return -EINVAL;

	if(!tidlist_remove_from(&ta->threads, tid)) return -EINVAL;

	void *utcb_loc = (void *)utcb_addr;
	free_threadno(tid);
	assert((L4_Word_t)utcb_loc >= L4_Address(ta->utcb_area));
	L4_Word_t pos = (L4_Word_t)utcb_loc - L4_Address(ta->utcb_area);
	assert(pos < L4_Size(ta->utcb_area));
	int u_slot = pos >> utcb_size_log2;
	assert(!bitmap_test_bit(ta->utcb_free, u_slot));
	bitmap_set_bit(ta->utcb_free, u_slot);

	L4_Word_t res = L4_ThreadControl(tid, L4_nilthread, L4_nilthread,
		L4_nilthread, (void *)-1);
	if(res != 1) {
		printf("uapi: deleting ThreadControl failed, res=%lu\n", res);
		abort();
	}

	if(ta->threads.size == 0) {
		printf("uapi: would delete pid=%d!\n", pid);
		/* TODO: zombify or wait(2) `pid'. */
	}

	if(is_self) muidl_raise_no_reply();
	return 0;
}


static int map_elf_image(
	int target_pid, const char *filename,
	L4_Word_t *lo_p, L4_Word_t *hi_p, L4_Word_t *start_addr_p)
{
	if(filename[0] != '/') return -ENOENT;
	while(*filename == '/') filename++;

	L4_Word_t handle = ~0ul;
	L4_ThreadId_t fs = L4_nilthread;

	/* FIXME: fetch rootfs using a canonical method */
	struct sneks_rootfs_info ri;
	int n = __info_rootfs_block(L4_Pager(), &ri);
	if(n != 0) goto end;
	fs.raw = ri.service;
	n = __fs_openat(fs, &handle, 0, filename, 0, 0);
	if(n != 0) {
		printf("openat failed, n=%d\n", n);
		goto end;
	}

	Elf32_Ehdr ehdr;
	unsigned ehdr_len = sizeof ehdr;
	n = __fs_read(fs, (void *)&ehdr, &ehdr_len, handle, 0, sizeof ehdr);
	if(n != 0) {
		printf("fs_read failed, n=%d\n", n);
		goto end;
	} else if(ehdr_len != sizeof ehdr) {
		printf("short read, got %u wanted %u\n",
			ehdr_len, (unsigned)sizeof ehdr);
		goto end;
	} else if(memcmp(&ehdr.e_ident, ELFMAG, SELFMAG) != 0) {
		printf("incorrect ELF magic!\n");
		goto end;
	}
	*start_addr_p = ehdr.e_entry;

	const Elf32_Phdr ep;
	unsigned phoff = ehdr.e_phoff;
	for(int i=0; i < ehdr.e_phnum; i++, phoff += ehdr.e_phentsize) {
		unsigned ep_len = sizeof ep;
		n = __fs_read(fs, (void *)&ep, &ep_len, handle, phoff, ep_len);
		if(n != 0 || ep_len < sizeof ep) {
			printf("fs_read failed or short, n=%d\n", n);
			goto end;
		}
		if(ep.p_type != PT_LOAD) continue;	/* skip GNU stack item */
		L4_Word_t addr = ep.p_vaddr;
		*lo_p = min(*lo_p, addr);
		*hi_p = max(*hi_p,
			(addr + ep.p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
		if(ep.p_filesz > 0) {
			n = __vm_mmap(vm_tid, target_pid, &addr, ep.p_filesz,
				PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_FILE,
				fs.raw, handle, ep.p_offset);
			if(n != 0) {
				printf("vm_mmap (file) failed, n=%d\n", n);
				goto end;
			}
		}
		size_t mapped = (ep.p_filesz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
		if(ep.p_memsz > mapped) {
			addr = ep.p_vaddr + mapped;
			size_t sz = (ep.p_memsz - mapped + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
			n = __vm_mmap(vm_tid, target_pid, &addr, sz,
				PROT_READ | PROT_WRITE, MAP_ANONYMOUS, L4_nilthread.raw,
				~0ul, 0);
			if(n != 0) {
				printf("vm_mmap (tail) failed, n=%d\n", n);
				goto end;
			}
		}
	}
	assert(n == 0);

end:
	/* FIXME: do copy that floppy. */
	// if(handle != ~0ul) __fs_close(fs, handle);
	return n <= 0 ? n : -EIO;
}


/* gets next userspace PID in the correct Unix fashion. allocates memory when
 * successful, or returns NULL.
 */
static struct process *alloc_process(int *pid_p)
{
	for(int iter=0; iter < SNEKS_MAX_PID; iter++) {
		int maybe = atomic_fetch_add_explicit(
			&next_user_pid, 1, memory_order_relaxed);
		if(maybe > SNEKS_MAX_PID) {
			bool yes = atomic_compare_exchange_strong(
				&next_user_pid, &maybe, 2);
			assert(yes || maybe <= SNEKS_MAX_PID);
			if(yes) maybe = 1;
		}
		struct process *p = ra_alloc(ra_process, maybe);
		if(p != NULL) {
			assert(ra_ptr2id(ra_process, p) == maybe);
			*pid_p = maybe;
			return p;
		}
	}
	return NULL;
}


/* release with free(). */
static inline bitmap *alloc_utcb_bitmap(L4_Fpage_t utcb_area) {
	return bitmap_alloc1(1 << (L4_SizeLog2(utcb_area) - utcb_size_log2));
}


static int desep(char *dst, const char *src)
{
	int i = 0;
	while(src[i] != '\0') {
		dst[i] = src[i] == 0x1e ? '\0' : src[i];
		i++;
	}
	dst[i] = '\0';
	dst[i + 1] = '\0';
	assert(strlen(src) == i);
	return i + 1;
}


static int cmp_fdlist_by_fd_desc(const void *ap, const void *bp) {
	const struct sneks_fdlist *a = ap, *b = bp;
	return (long)b->fd - (long)a->fd;
}


static int make_fdlist_page(
	void **ptr, int *n_pages_p,
	const L4_Word_t *fd_servs, unsigned fd_servs_len,
	const L4_Word_t *fd_cookies, unsigned fd_cookies_len,
	const int32_t *fd_fds, unsigned fd_fds_len)
{
	int n_fds = min(min(fd_fds_len, fd_cookies_len), fd_servs_len);
	if(n_fds == 0) {
		*ptr = NULL;
		*n_pages_p = 0;
		return 0;
	}
	struct sneks_fdlist *list;
	size_t alloc = (n_fds * sizeof *list + PAGE_SIZE - 1) & ~PAGE_MASK;
	list = aligned_alloc(PAGE_SIZE, alloc);
	if(list == NULL) return -ENOMEM;
	memset(list + n_fds, '\0', alloc - n_fds * sizeof *list);
	for(int i=0; i < n_fds; i++) {
		list[i] = (struct sneks_fdlist){
			.next = sizeof *list, .fd = fd_fds[i],
			.cookie = fd_cookies[i],
			.serv.raw = fd_servs[i],
		};
	}
	qsort(list, n_fds, sizeof *list, &cmp_fdlist_by_fd_desc);
	for(int i=1; i < n_fds; i++) {
		if(list[i - 1].fd == list[i].fd) {
			free(list);
			return -EINVAL;
		}
	}
	*ptr = list;
	*n_pages_p = alloc / PAGE_SIZE;
	return 0;
}


static int uapi_spawn(
	const char *filename,
	const char *argbuf, const char *envbuf,
	const L4_Word_t *fd_servs, unsigned fd_servs_len,
	const L4_Word_t *fd_cookies, unsigned fd_cookies_len,
	const int32_t *fd_fds, unsigned fd_fds_len)
{
	printf("%s: entered! filename=`%s'\n", __func__, filename);
	assert(!L4_IsNilThread(vm_tid));

	int newpid;
	struct process *p = alloc_process(&newpid);
	if(p == NULL) return -ENOMEM;
	assert(newpid > 0 && newpid <= SNEKS_MAX_PID);

	int n = __vm_fork(vm_tid, 0, newpid);
	if(n != 0) {
		p->task.utcb_area = L4_Nilpage;
		ra_free(ra_process, p);
		goto fail;
	}

	L4_Word_t lo = ~0ul, hi = 0, start_addr = ~0ul;
	n = map_elf_image(newpid, filename, &lo, &hi, &start_addr);
	if(n != 0) {
		/* FIXME: destroy `p', its space on vm */
		goto fail;
	}
	int n_threads = 1024, utcb_size = 512;	/* FIXME: get from KIP etc */
	L4_Word_t ua_size = 1ul << size_to_shift(utcb_size * n_threads),
		resv_size = ua_size + L4_KipAreaSize(the_kip) + PAGE_SIZE,
		resv_start;
	if(resv_size >= lo - 0x10000) {		/* preserve low 64k */
		/* can't fit in low space; set them up after "hi" instead. userspace
		 * crt should initialize its sbrk after the sysinfo page.
		 */
		resv_start = (hi + ua_size - 1) & ~(ua_size - 1);
		assert(resv_start > hi);
	} else {
		/* low space. nice! */
		resv_start = ((lo - resv_size) & ~(ua_size - 1));
		assert(resv_start >= 0x10000);
		assert(resv_start + resv_size < lo);
	}
	p->task.utcb_area = L4_Fpage(resv_start, ua_size);
	p->task.kip_area = L4_Fpage(resv_start + ua_size,
		L4_KipAreaSize(the_kip));
	printf("%s: utcb_area=%#lx:%#lx, kip_area=%#lx:%#lx\n", __func__,
		L4_Address(p->task.utcb_area), L4_Size(p->task.utcb_area),
		L4_Address(p->task.kip_area), L4_Size(p->task.kip_area));
	darray_init(p->task.threads);
	p->task.utcb_free = alloc_utcb_bitmap(p->task.utcb_area);
	if(p->task.utcb_free == NULL) {
		/* FIXME: cleanup */
		printf("%s: can't allocate utcb bitmap\n", __func__);
		n = -ENOMEM;
		goto fail;
	}

	L4_Word_t resvhi;
	n = __vm_configure(vm_tid, &resvhi, newpid, p->task.utcb_area,
		p->task.kip_area);
	if(n != 0) {
		/* FIXME: cleanup */
		goto fail;
	}
	assert(resvhi < lo);

	/* compose and deliver args, env */
	L4_Word_t argpos = (hi + PAGE_SIZE - 1) & ~PAGE_MASK;
	const char *bufs[] = { argbuf, envbuf };
	void *argtmp = malloc(max(strlen(argbuf), strlen(envbuf)) + 8);
	for(int i=0; i < ARRAY_SIZE(bufs); i++) {
		int len = desep(argtmp, bufs[i]);
		n = __vm_upload_page(vm_tid, newpid, argpos, argtmp, len);
		if(n != 0) {
			free(argtmp);
			/* FIXME: cleanup */
			goto fail;
		}
		argpos += (len + PAGE_SIZE - 1) & ~PAGE_MASK;
	}
	free(argtmp);

	/* also the fdlist. */
	L4_Word_t fdlist_start = 0;
	void *fd_pages;
	int n_fdlist_pages;
	n = make_fdlist_page(&fd_pages, &n_fdlist_pages,
		fd_servs, fd_servs_len, fd_cookies, fd_cookies_len,
		fd_fds, fd_fds_len);
	if(n != 0) goto fail;	/* FIXME: cleanup */
	if(n_fdlist_pages > 0) {
		fdlist_start = argpos;
		for(int i=0; i < n_fdlist_pages; i++, argpos += PAGE_SIZE) {
			n = __vm_upload_page(vm_tid, newpid, argpos,
				fd_pages + i * PAGE_SIZE, PAGE_SIZE);
			if(n != 0) {
				/* FIXME: cleanup */
				goto fail;
			}
		}
	}
	free(fd_pages);

	/* create first thread and address space. */
	void *utcb_loc;
	L4_ThreadId_t first_tid = allocate_thread(newpid, &utcb_loc);
	if(L4_IsNilThread(first_tid)) {
		printf("%s: allocate_thread() failed!\n", __func__);
		abort();	/* FIXME: cleanup and return error */
	}
	n = make_space(first_tid, p->task.kip_area, p->task.utcb_area);
	if(n < 0) {
		printf("%s: make_space() failed, n=%d\n", __func__, n);
		/* FIXME: cleanup */
		goto fail;
	}
	L4_Word_t res = L4_ThreadControl(first_tid, first_tid,
		L4_Myself(), vm_tid, utcb_loc);
	if(res != 1) {
		printf("%s: ThreadControl failed, ec=%lu\n", __func__, L4_ErrorCode());
		abort();	/* FIXME: cleanup and return error */
	}

	/* send it. */
	L4_Word_t status;
	n = __vm_breath_of_life(vm_tid, &status, first_tid.raw,
		fdlist_start, start_addr);
	if(n < 0) {
		printf("%s: VM::breath_of_life() failed, n=%d\n", __func__, n);
		/* FIXME: cleanup */
		goto fail;
	} else if(status != 0) {
		printf("%s: VM::breath_of_life() failed, inner ec=%lu\n",
			__func__, status);
		/* FIXME: cleanup */
		goto fail;
	}

	return newpid;

fail:
	return n <= 0 ? n : -EIO;
}


static int uapi_kill(int pid, int sig)
{
	printf("%s: called, pid=%d, sig=%d\n", __func__, pid, sig);

	/* FIXME: all of these for things besides systask-signaled segfaults. */
	if(sig != SIGSEGV) return -EINVAL;
	if(!IS_SYSTASK(pidof_NP(muidl_get_sender()))) return -EPERM;

	struct process *p = get_process(pid);
	if(p == NULL) return -ESRCH;

	/* TODO: consider signal delivery someday */
	printf("%s: destroying process.\n", __func__);
	destroy_process(p);

	return 0;
}


static void uapi_exit(int status)
{
	int pid = pidof_NP(muidl_get_sender());
	if(IS_SYSTASK(pid)) {
		printf("%s: not implemented for systasks (pid=%d)\n", __func__, pid);
		/* but should it be? */
	} else {
		struct process *p = get_process(pid);
		assert(p != NULL);
		destroy_process(p);	/* TODO: leave crumbs or signal wait(2)ers */
		muidl_raise_no_reply();
	}
}


static int uapi_wait(
	int32_t *si_pid_p, int32_t *si_uid_p, int32_t *si_signo_p,
	int32_t *si_status_p, int32_t *si_code_p,
	int32_t idtype, int32_t id, int32_t options)
{
	return -ENOSYS;
}


static int uapi_fork(L4_Word_t *tid_raw_p, L4_Word_t sp, L4_Word_t ip)
{
	return -ENOSYS;
}


int uapi_loop(void *param_ptr)
{
	uapi_tid = L4_Myself();

	static const struct root_uapi_vtable vtab = {
		.create_thread = &uapi_create_thread,
		.remove_thread = &uapi_remove_thread,
		.spawn = &uapi_spawn,
		.kill = &uapi_kill,
		.exit = &uapi_exit,
		.wait = &uapi_wait,
		.fork = &uapi_fork,
	};
	for(;;) {
		L4_Word_t st = _muidl_root_uapi_dispatch(&vtab);
		if(st == MUIDL_UNKNOWN_LABEL) {
			/* ignore it. in debug this'd indicate an out-of-sync IPC flow,
			 * which would get confused by any reply. so ignoring will
			 * minimize observed weirdness.
			 */
		} else if(st != 0 && !MUIDL_IS_L4_ERROR(st)) {
			printf("%s: dispatch status %#lx (last tag %#lx)\n",
				__func__, st, muidl_get_tag().raw);
		}
	}
	return -1;
}


COLD void uapi_init(void)
{
	utcb_size_log2 = size_to_shift(L4_UtcbSize(the_kip));
	assert(1 << utcb_size_log2 >= L4_UtcbSize(the_kip));
	ra_systask = RA_NEW(struct systask, 1 << 14);	/* fewer than 16k */
	ra_process = RA_NEW(struct process, 1 << 15);
	ra_disable_id_0(ra_process);	/* there's no PID 0. */

	for(int i=0; i < 4; i++) {
		tno_free_maps[i] = bitmap_alloc1(TNOS_PER_BITMAP);
		if(tno_free_maps[i] == NULL) {
			printf("%s: can't allocate threadno bitmap %d\n", __func__, i);
			abort();
		}
		tno_free_counts[i] = TNOS_PER_BITMAP;
	}

	/* burn the first 512 thread numbers to protect the forbidden range.
	 * TODO: this should be done according to a parameter, or a KIP value.
	 */
	for(int i=0; i < 512; i++) {
		bitmap_clear_bit(tno_free_maps[0], i);
	}
	tno_free_counts[0] -= 512;
}
