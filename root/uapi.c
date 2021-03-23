
/* userspace API portion of the root task. */

#define ROOTUAPI_IMPL_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdnoreturn.h>
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
#include <ccan/list/list.h>

#include <l4/types.h>
#include <l4/ipc.h>
#include <l4/thread.h>
#include <ukernel/rangealloc.h>
#include <sneks/hash.h>
#include <sneks/mm.h>
#include <sneks/msg.h>
#include <sneks/rollback.h>
#include <sneks/process.h>
#include <sneks/api/vm-defs.h>
#include <sneks/api/proc-defs.h>
#include <sneks/sys/msg-defs.h>
#include <sneks/sys/info-defs.h>

#include "muidl.h"
#include "root-impl-defs.h"
#include "elf.h"
#include "defs.h"


#define MAX_PID 0xffff
#define NUM_SYSTASK_IDS (1 << 14)	/* fewer than 16k */

#define TNOS_PER_BITMAP (1 << 16)


/* system tasks. allocated in ra_systask. note that ra_systask's id=0 is
 * available and maps to SNEKS_MIN_SYSID + 0.
 */
struct systask
{
	struct task_common task;
	/* (systask scheduling things would go here eventually) */
};


static size_t hash_waitany_tid(const void *ptr, void *priv);
static size_t hash_ppid(const void *ptr, void *priv);


struct rangealloc *ra_process = NULL;
static struct rangealloc *ra_systask = NULL;
static int utcb_size_log2;
L4_ThreadId_t uapi_tid;

/* TID allocator. this is for the 32-bit mode where the two highest bits of
 * ThreadNo are the two highest bits of the process ID.
 */
static bitmap *tno_free_maps[4];
static int tno_free_counts[4];

/* PID allocator, backed by rangealloc. */
static _Atomic int next_user_pid = 1;

/* pid-to-children multimap. used to return -ECHILD in waitid(P_ANY, ...). */
static struct htable pid_to_child_hash = HTABLE_INITIALIZER(
	pid_to_child_hash, &hash_ppid, NULL);

/* processes that have PF_WAIT_ALL set will have at least one thread in
 * waitany_hash, and ones where the bit is clear have none. its members are
 * L4_ThreadId_t cast to <void *> and hashed by their process IDs.
 */
static struct htable waitany_hash = HTABLE_INITIALIZER(
	waitany_hash, &hash_waitany_tid, NULL);



static size_t hash_waitany_tid(const void *ptr, void *priv) {
	L4_ThreadId_t t = { .raw = (L4_Word_t)ptr };
	return int_hash(pidof_NP(t));
}


static size_t hash_ppid(const void *ptr, void *priv) {
	const struct process *p = ptr;
	return int_hash(p->ppid);
}

static bool cmp_ppid_to_int(const void *cand, void *key) {
	const struct process *p = cand;
	return p->ppid == *(int *)key;
}


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
	assert(!bitmap_test_bit(tno_free_maps[map], bit));
	bitmap_set_bit(tno_free_maps[map], bit);
	tno_free_counts[map]++;
	assert(tno_free_counts[map] <= TNOS_PER_BITMAP);
}


struct process *get_process(int pid)
{
	if(pid <= 0 || pid > SNEKS_MAX_PID) return NULL;
	sync_confirm();
	/* no need to use the _safe variant since the rangealloc is backed by
	 * virtual memory, which initializes such that p->task.utcb_area will be
	 * L4_Nilpage for PIDs landing on unallocated pages.
	 */
	struct process *p = ra_id2ptr(ra_process, pid);
	if(p == NULL || L4_IsNilFpage(p->task.utcb_area)) return NULL;
	return p;
}


static struct systask *get_systask(int pid)
{
	int stid = pid - SNEKS_MIN_SYSID;
	if(stid < 0 || stid >= NUM_SYSTASK_IDS) return NULL;
	sync_confirm();
	/* see comment about safety in get_process() */
	struct systask *st = ra_id2ptr(ra_systask, stid);
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


/* release with free(). */
static bitmap *alloc_utcb_bitmap(L4_Fpage_t utcb_area) {
	return bitmap_alloc1(1 << (L4_SizeLog2(utcb_area) - utcb_size_log2));
}


static bool task_common_ctor(
	struct task_common *task, L4_Fpage_t kip_area, L4_Fpage_t utcb_area)
{
	task->utcb_area = L4_Nilpage;
	task->utcb_free = alloc_utcb_bitmap(utcb_area);
	if(task->utcb_free == NULL) return false;
	task->kip_area = kip_area;
	task->utcb_area = utcb_area;
	darray_init(task->threads);
	return true;
}


/* destroys threads, releases tno bitmap slots, clears the tidlist, tosses
 * associated memory, but doesn't invalidate the structure.
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

	/* idempotent memory release */
	darray_free(task->threads); darray_init(task->threads);
	free(task->utcb_free); task->utcb_free = NULL;
	assert(!L4_IsNilFpage(task->utcb_area));

	/* deconfigure virtual memory as well */
	int n = __vm_erase(vm_tid, ra_ptr2id(ra_process,
		container_of(task, struct process, task)));
	if(n != 0) {
		printf("root: VM::erase failed, n=%d\n", n);
		/* not a lot we can do besides that */
	}
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

		/* roottask initialization. this should execute in the thread that
		 * enters main() and no other. (after it moves out of the forbidden
		 * range, of course.)
		 */
		int tno = L4_ThreadNo(L4_MyGlobalId());
		for(int i=0; i < next_early_utcb_slot; i++) {
			L4_ThreadId_t tid = L4_GlobalId(tno + i,
				L4_Version(L4_MyGlobalId()));
			if(L4_IsNilThread(L4_LocalIdOf(tid))) continue;
			assert(bitmap_test_bit(st->task.utcb_free, i));
			bitmap_clear_bit(st->task.utcb_free, i);
			darray_push(st->task.threads, tid);
		}
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


/* TODO: drop @pid, replace with struct task_common ptr since all callers
 * already have one.
 */
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
	assert(bitmap_test_bit(tno_free_maps[map], bit));
	bitmap_clear_bit(tno_free_maps[map], bit);
	assert(tno_free_counts[map] > 0);
	tno_free_counts[map]--;

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
		assert(!bitmap_test_bit(tno_free_maps[map], bit));
		bitmap_set_bit(tno_free_maps[map], bit);
		tno_free_counts[map]++;
		assert(tno_free_counts[map] <= TNOS_PER_BITMAP);
		return L4_nilthread;
	}
	bitmap_clear_bit(task->utcb_free, utcb_slot);
	darray_push(task->threads, tid);

	*utcb_loc_p = (void *)(L4_Address(task->utcb_area)
		+ (utcb_slot << utcb_size_log2));
	return tid;
}


/* for rolling back allocate_thread() when make_space() fails, removing
 * spent threadlets, and eventually for implementing Proc::remove_thread.
 */
static void free_thread(
	struct task_common *task,
	L4_ThreadId_t tid, void *utcb_loc)
{
	L4_Word_t pos = (L4_Word_t)utcb_loc - L4_Address(task->utcb_area),
		slot = pos >> utcb_size_log2;
	assert((L4_Word_t)utcb_loc >= L4_Address(task->utcb_area)
		&& pos < L4_Size(task->utcb_area));
	assert(!bitmap_test_bit(task->utcb_free, slot));
	bitmap_set_bit(task->utcb_free, slot);
	tidlist_remove_from(&task->threads, tid);
}


/* destroy a zombie process. task_common_dtor(&@p->task) should already have
 * been called from zombify(); this one removes references and invalidates the
 * structure.
 */
static void destroy_process(struct process *p)
{
	/* TODO: various other final process destruction things, such as
	 * reparenting of children to PID1 and what-not.
	 */
	htable_del(&pid_to_child_hash, int_hash(p->ppid), p);
	p->task.utcb_area = L4_Nilpage;
	ra_free(ra_process, p);
	assert(get_process(ra_ptr2id(ra_process, p)) == NULL);
}


/* TODO: make the code, signo, status triple parameters to this function,
 * since all callsites set those anyway.
 */
void zombify(struct process *p)
{
	task_common_dtor(&p->task);

	struct process *parent = get_process(p->ppid);
	if(parent == NULL) {
		/* FIXME holy shit move this into a header */
		extern noreturn void panic(const char *croak);
		int pid = ra_ptr2id(ra_process, p);
		if(pid == 1) {
			printf("init died? exitcode=%#08x\n", p->code);
			panic("it's wholly improper for init(8) to exit (not syncing)");
		} else {
			if(p->ppid != 0) {
				printf("warning: exiting pid=%d had ->ppid=%d, which doesn't exist\n",
					pid, p->ppid);
			}
			/* reparent it to init, see what happens. */
			p->ppid = 1;
			parent = get_process(1);
			if(parent == NULL) panic("no init? shame on you");
			assert(parent != p);	/* roll along, ouroboros */
		}
	}

	/* NOTE: there's a question about whether sigchld should be sent when the
	 * exiting child is immediately caught in a waitid(). sneks does, because
	 * SIGCHLD handlers are supposed to use WNOHANG and deal with spurious
	 * signals if waitid() is called from somewhere else also.
	 */
	sig_send(parent, SIGCHLD, false);

	L4_ThreadId_t waiter = L4_nilthread;
	if(!L4_IsNilThread(p->wait_tid)) waiter = p->wait_tid;
	else if(parent->flags & PF_WAIT_ANY) {
		size_t hash = int_hash(p->ppid);
		struct htable_iter it;
		bool more = false;
		for(void *cur = htable_firstval(&waitany_hash, &it, hash);
			cur != NULL;
			cur = htable_nextval(&waitany_hash, &it, hash))
		{
			L4_ThreadId_t cand = { .raw = (L4_Word_t)cur };
			assert(!L4_IsNilThread(cand));
			if(pidof_NP(cand) == p->ppid) {
				if(L4_IsNilThread(waiter)) {
					waiter = cand;
					htable_delval(&waitany_hash, &it);
				} else {
					more = true;
					break;
				}
			}
		}
		if(!more) {
			parent->flags &= ~PF_WAIT_ANY;
			/* TODO: under !NDEBUG, check that there truly aren't any more. */
		}
	}

	assert(!L4_IsNilThread(sysmsg_tid));
	L4_Word_t msg[] = {
		ra_ptr2id(ra_process, p),
		MPL_EXIT | p->signo << 8, p->status, p->code,
	};
	bool dummy;
	int n = __sysmsg_broadcast(sysmsg_tid, &dummy,
		1 << MSGB_PROCESS_LIFECYCLE, 0, msg, ARRAY_SIZE(msg));
	if(n != 0) {
		printf("%s: Sysmsg::broadcast failed, n=%d\n", __func__, n);
	}

	if(!L4_IsNilThread(waiter)) {
		/* send reply for Proc::wait. */
		L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 5 }.raw);
		L4_LoadMR(1, ra_ptr2id(ra_process, p));
		L4_LoadMR(2, 0);	/* FIXME: use ta->u.uid or some such */
		L4_LoadMR(3, p->signo);
		L4_LoadMR(4, p->status);
		L4_LoadMR(5, p->code);
		L4_MsgTag_t tag = L4_Reply(waiter);
		if(L4_IpcSucceeded(tag)) {
			destroy_process(p);
			return;
		}
		/* discard the waiter on failure; it'll have stopped listening. */
	}

	/* add a reapable zombie. */
	list_add_tail(&parent->dead_list, &p->dead_link);
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
		printf("%s: ... tid=%lu:%lu, space=%lu:%lu, utcb_loc=%p\n", __func__,
			L4_ThreadNo(tid), L4_Version(tid),
			L4_ThreadNo(space), L4_Version(space), utcb_loc);
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


#ifdef BUILD_SELFTEST
/* NOTE: the next_skip stuff is very untested. */
static int uapi_get_systask_threads(
	L4_Word_t *tidlist_out, unsigned *tidlist_len_p,
	int min_pid, int skip)
{
	*tidlist_len_p = 0;
	int next_skip = 0;
	struct ra_iter it;
	for(struct systask *t = ra_first(ra_systask, &it);
		t != NULL && *tidlist_len_p < 63;
		t = ra_next(ra_systask, &it))
	{
		if(min_pid > SNEKS_MIN_SYSID + ra_ptr2id(ra_systask, t)) continue;
		tidlist *tids = &t->task.threads;
		next_skip = tids->size;
		for(int i=0; i < tids->size && *tidlist_len_p < 63; i++) {
			next_skip--;
			if(skip-- > 0) continue;
			tidlist_out[(*tidlist_len_p)++] = tids->item[i].raw;
		}
		skip = 0;
	}

	return next_skip;
}
#endif


static int map_elf_image(
	int target_pid, const char *filename,
	L4_Word_t *lo_p, L4_Word_t *hi_p, L4_Word_t *start_addr_p)
{
	if(filename[0] != '/') return -ENOENT;
	while(filename[1] == '/') filename++;

	FILE *file = fopen(filename, "rb");
	if(file == NULL) return -errno;

	int handle = fhandle_NP(file);
	L4_ThreadId_t fs = fserver_NP(file);

	Elf32_Ehdr ehdr;
	size_t n = fread(&ehdr, sizeof ehdr, 1, file);
	if(n != 1) {
		printf("%s: fread() failed\n", __func__);
		goto end;
	} else if(memcmp(&ehdr.e_ident, ELFMAG, SELFMAG) != 0) {
		printf("%s: incorrect ELF magic!\n", __func__);
		errno = EINVAL;
		goto end;	/* go sip from thy leaden goblet, knife ears */
	}
	*start_addr_p = ehdr.e_entry;

	unsigned phoff = ehdr.e_phoff;
	for(int i=0; i < ehdr.e_phnum; i++, phoff += ehdr.e_phentsize) {
		Elf32_Phdr ep;
		n = fread(&ep, sizeof ep, 1, file);
		if(n != 1) {
			printf("%s: fread() failed or short\n", __func__);
			goto end;
		}
		if(ep.p_type != PT_LOAD) continue;	/* skip GNU stack item */
		L4_Word_t addr = ep.p_vaddr;
		*lo_p = min(*lo_p, addr);
		*hi_p = max(*hi_p,
			(addr + ep.p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
		if(ep.p_filesz > 0) {
			if(ep.p_offset & PAGE_MASK) {
				printf("%s: PT_LOAD offset %#x not aligned to page\n",
					__func__, ep.p_offset);
				n = -EINVAL;
				goto end;
			}
			n = __vm_mmap(vm_tid, target_pid, &addr, ep.p_filesz,
				PROT_READ | PROT_WRITE | PROT_EXEC,
				MAP_PRIVATE | MAP_FILE | MAP_FIXED,
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
			n = __vm_mmap(vm_tid, target_pid, &addr, sz, PROT_READ | PROT_WRITE,
				MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED,
				L4_nilthread.raw, ~0ul, 0);
			if(n != 0) {
				printf("vm_mmap (tail) failed, n=%d\n", n);
				goto end;
			}
		}
	}

end:
	/* FIXME: arrange for VM to dup the handle, then fclose() the file
	 * properly. "do copy that floppy," as it previously said.
	 */
	//fclose(file);
	return -errno;
}


static void process_ctor(struct process *p)
{
	list_head_init(&p->dead_list);
	p->wait_tid = L4_nilthread;
	p->sigpage_addr = 0;
	p->ign_set = 0;
	p->dfl_set = ~(uint64_t)0;
	p->mask_set = 0;
	p->pending_set = 0;
	p->sighelper_tid = L4_nilthread;
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
			process_ctor(p);
			return p;
		}
	}
	return NULL;
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
	size_t alloc = ((n_fds + 1) * sizeof *list + PAGE_SIZE - 1) & ~PAGE_MASK;
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
	assert(list[n_fds].next == 0);	/* terminated w/ memset */
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
	assert(!L4_IsNilThread(vm_tid));

	int newpid;
	struct process *p = alloc_process(&newpid);
	if(p == NULL) return -ENOMEM;
	assert(newpid > 0 && newpid <= SNEKS_MAX_PID);

	L4_ThreadId_t caller = muidl_get_sender();
	if(IS_SYSTASK(pidof_NP(caller))) {
		/* systask spawn always start at root privilege. this could be
		 * specified otherwise so that root-privileged processes aren't
		 * created willy nilly.
		 */
		p->real_uid = p->eff_uid = p->saved_uid = 0;
		/* TODO: same for gids */
		p->ppid = 0;
	} else {
		struct process *parent = get_process(pidof_NP(caller));
		if(parent == NULL) {
			printf("%s: can't find parent process (caller=%#lx:%#lx, pid=%u)\n",
				__func__, L4_ThreadNo(caller), L4_Version(caller),
				pidof_NP(caller));
			p->task.utcb_area = L4_Nilpage;
			ra_free(ra_process, p);
			return -EINVAL;
		}

		p->real_uid = parent->real_uid;
		p->eff_uid = parent->eff_uid;
		/* TODO: saved_uid, same for gid. or maybe stick 'em in a struct and
		 * assign that.
		 */
		p->ppid = pidof_NP(caller);
	}

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
		n = __vm_upload_page(vm_tid, newpid, argpos, argtmp, len,
			PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED);
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
				fd_pages + i * PAGE_SIZE, PAGE_SIZE,
				PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED);
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


void sig_remove_helper(struct process *p)
{
	L4_Word_t ret = L4_ThreadControl(p->sighelper_tid, L4_nilthread,
		L4_nilthread, L4_nilthread, (void *)-1);
	if(ret != 1) {
		printf("uapi:%s: deleting threadctl failed, ec=%lu\n", __func__,
			L4_ErrorCode());
		abort();	/* FIXME: do something else instead */
	}
	free_thread(&p->task, p->sighelper_tid, p->sighelper_utcb);
	free_threadno(p->sighelper_tid);
	p->sighelper_tid = L4_nilthread;
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
		p->code = CLD_EXITED; p->status = status; p->signo = 0;
		zombify(p);
		muidl_raise_no_reply();
	}
}


static bool has_children(int pid) {
	return htable_get(&pid_to_child_hash, int_hash(pid),
		&cmp_ppid_to_int, &pid) != NULL;
}


static void call_destroy_process(L4_Word_t dead_ptr, void *self_ptr)
{
	struct process *self = self_ptr, *dead = (struct process *)dead_ptr;
	list_del_from(&self->dead_list, &dead->dead_link);
	destroy_process(dead);
}


static int uapi_wait(
	int32_t *si_pid_p, int32_t *si_uid_p, int32_t *si_signo_p,
	int32_t *si_status_p, int32_t *si_code_p,
	int32_t idtype, int32_t id, int32_t options)
{
	int caller = pidof_NP(muidl_get_sender());
	if(IS_SYSTASK(caller)) return -EINVAL;	/* maybe one day? */

	struct process *self = get_process(caller), *dead = NULL, *live = NULL;
	switch(idtype) {
		case P_PID:
			dead = get_process(id);
			if(dead == NULL || dead->ppid != caller) return -ECHILD;
			if(dead->task.threads.size > 0) { live = dead; dead = NULL; }
			break;

		case P_ANY:
			dead = list_top(&self->dead_list, struct process, dead_link);
			if(dead == NULL && !has_children(caller)) return -ECHILD;
			assert(dead == NULL || dead->ppid == caller);
			break;

		default:
		case P_PGID:
			printf("%s: unsupported idtype=%d\n", __func__, idtype);
			return -EINVAL;	/* TODO */
	}
	if(dead != NULL) {
		assert(live == NULL);
		*si_pid_p = ra_ptr2id(ra_process, dead);
		*si_uid_p = 0;	/* FIXME: dead->uid or something */
		*si_signo_p = dead->signo;
		*si_status_p = dead->status;
		*si_code_p = dead->code;
		set_confirm(&call_destroy_process, (L4_Word_t)dead, self);
		return 0;
	} else if(live != NULL) {
		assert(dead == NULL);
		if((options & WNOHANG) != 0) goto nohang;
		/* FIXME: this breaks when multiple threads on the parent process wait
		 * on `live' at the same time, because all will sleep but just one
		 * wakes up.
		 */
		live->wait_tid = muidl_get_sender();
		muidl_raise_no_reply();
		return 0;
	} else if((options & WNOHANG) == 0) {
		assert(idtype == P_ANY);
		/* TODO: this may fail in htable_add(), but the wait(2) family doesn't
		 * allow for -ENOMEM return value. in practice it's very unlikely to
		 * do so since 32-bit L4.X2 permits up to 256k threads which would
		 * consume up to 3 meg of RAM during the final resize. so it's this,
		 * or allocating a list link for every thread that might ever call
		 * wait(2); that may yet come to be if it turns out that threads
		 * require more tracking than just the identifier.
		 */
		self->flags |= PF_WAIT_ANY;
		L4_ThreadId_t caller = muidl_get_sender();
		bool ok = htable_add(&waitany_hash, int_hash(pidof_NP(caller)),
			(void *)caller.raw);
		if(!ok) {
			/* FIXME: proper handling, though failure is very unlikely. */
			printf("%s: htable_add failed\n", __func__);
			abort();
		}
		muidl_raise_no_reply();
		return 0;
	} else {
nohang:
		/* don't hang. */
		*si_pid_p = 0; *si_uid_p = 0;
		*si_signo_p = 0; *si_status_p = 0; *si_code_p = 0;
		return 0;
	}

	return -ENOSYS;
}


static int uapi_fork(L4_Word_t *tid_raw_p, L4_Word_t sp, L4_Word_t ip)
{
	int pid = pidof_NP(muidl_get_sender());
	if(IS_SYSTASK(pid)) return -EINVAL;	/* fuck off! */

	struct process *src = get_process(pid);
	assert(src != NULL);	/* per control of PID assignment */

	int newpid;
	struct process *dst = alloc_process(&newpid);
	if(dst == NULL) return -EAGAIN;
	assert(newpid > 0);
	assert(!IS_SYSTASK(newpid));

	darray_init(dst->task.threads);
	dst->task.utcb_area = src->task.utcb_area;
	dst->task.kip_area = src->task.kip_area;
	dst->task.utcb_free = alloc_utcb_bitmap(dst->task.utcb_area);
	if(dst->task.utcb_free == NULL) {
		/* FIXME: cleanup */
		return -ENOMEM;
	}
	dst->ppid = pid;
	dst->real_uid = src->real_uid;
	dst->eff_uid = src->eff_uid;
	dst->saved_uid = src->saved_uid;
	dst->sigpage_addr = src->sigpage_addr;
	dst->ign_set = src->ign_set;
	dst->dfl_set = src->dfl_set;
	dst->mask_set = src->mask_set;
	dst->pending_set = 0;
	bool ok = htable_add(&pid_to_child_hash, int_hash(pid), dst);
	if(!ok) {
		/* FIXME: cleanup */
		return -ENOMEM;
	}

	int n = __vm_fork(vm_tid, pid, newpid);
	if(n != 0) {
		/* FIXME: cleanup */
		if(n > 0) return -EIO; else return n;
	}

	assert(!L4_IsNilThread(sysmsg_tid));
	L4_Word_t msg[] = { pid, MPL_FORK | newpid << 8 };
	bool dummy;
	n = __sysmsg_broadcast(sysmsg_tid, &dummy,
		1 << MSGB_PROCESS_LIFECYCLE, 0, msg, ARRAY_SIZE(msg));
	if(n != 0) {
		printf("root: warning: fork broadcast failed, n=%d\n", n);
	}

	void *utcb_loc;
	L4_ThreadId_t start_tid = allocate_thread(newpid, &utcb_loc);
	if(L4_IsNilThread(start_tid)) {
		/* FIXME: shouldn't happen */
		printf("can't allocate_thread() on fresh space?\n");
		abort();
	}
	n = make_space(start_tid, dst->task.kip_area, dst->task.utcb_area);
	if(n != 0) {
		/* FIXME: cleanup */
		printf("can't make space in fork?\n");
		abort();
	}
	L4_Word_t res = L4_ThreadControl(start_tid, start_tid, L4_Myself(),
		vm_tid, utcb_loc);
	if(res != 1) {
		/* FIXME: cleanup */
		printf("%s: ThreadControl failed, ec=%lu\n", __func__,
			L4_ErrorCode());
		abort();
	}
	n = __vm_breath_of_life(vm_tid, &res, start_tid.raw, sp, ip);
	if(n != 0 || res != 0) {
		/* FIXME: cleanup */
		printf("%s: VM::breath_of_life failed, n=%d, res=%lu\n", __func__,
			n, res);
		abort();
	}

	*tid_raw_p = start_tid.raw;
	return newpid;
}


static void uapi_getresugid(
	uid_t *r_uid, uid_t *e_uid, uid_t *s_uid,
	gid_t *r_gid, gid_t *e_gid, gid_t *s_gid)
{
	struct process *p = get_process(pidof_NP(muidl_get_sender()));
	if(p == NULL) {
		*r_uid = *e_uid = *s_uid = ~0u;
		*r_gid = *e_gid = *s_gid = ~0u;
		return;
	}

	*r_uid = p->real_uid; *e_uid = p->eff_uid; *s_uid = p->saved_uid;
	/* TODO */
	*r_gid = ~0u; *e_gid = ~0u; *s_gid = ~0u;
}


static int uapi_setresugid(
	int16_t mode,
	uid_t r_uid, uid_t e_uid, uid_t s_uid,
	gid_t r_gid, gid_t e_gid, gid_t s_gid)
{
	struct process *p = get_process(pidof_NP(muidl_get_sender()));
	if(p == NULL) return -ESRCH; /* sneks extension for no process found */

	if(mode < 1 || mode > 3) return -EINVAL;
	else if(mode == 1) {
		if(p->eff_uid == 0) {
			/* "if the calling process is privileged, the real UID and saved
			 * set-user-ID are also set." here it's assumed that "saved
			 * set-user-ID" means saved UID.
			 *
			 * TODO: do this also when @p got suid-root via exec.
			 */
			e_uid = r_uid;
			s_uid = r_uid;
		} else if(r_uid == p->eff_uid || r_uid == p->saved_uid) {
			e_uid = r_uid;
			r_uid = -1;
			assert(s_uid == -1);
		} else {
			return -EPERM;
		}
		/* TODO: 1-parameter mode for gids */
	} else if(mode == 2) {
		/* TODO */
		return -ENOSYS;
	} else if(mode == 3 && p->eff_uid != 0) {
		/* validate when not in root mode. */
		const uint32_t cands[] = { r_uid, e_uid, s_uid };
		for(int i=0; i < ARRAY_SIZE(cands); i++) {
			uint32_t c = cands[i];
			if(c == -1) continue;
			if(c != p->real_uid && c != p->eff_uid && c != p->saved_uid) {
				return -EPERM;	/* DENIED */
			}
		}
		/* TODO: same for gids */
	}

	/* assign. */
	if(r_uid != -1) p->real_uid = r_uid;
	if(e_uid != -1) p->eff_uid = e_uid;
	if(s_uid != -1) p->saved_uid = s_uid;
	/* TODO: same for gids */

	return 0;
}


static int uapi_resolve(
	unsigned *object_p, L4_Word_t *server_raw_p,
	int *ifmt_p, L4_Word_t *cookie_p,
	L4_Word_t dirfd, const char *path, int flags)
{
	if(dirfd != 0) return -EBADF;
	if(path[0] != '/') return -ENOENT;

	/* TODO: drop this once fs.squashfs no longer forces /initrd up front,
	 * before end of wip/dev-nodes.
	 */
	char realpath[strlen(path) + 8];
	memcpy(realpath, "/initrd", 7);
	memcpy(realpath + 7, path, strlen(path) + 1);

	/* TODO: replace with muidl propagation */
	L4_MsgTag_t tag = { .X.label = 0xe808, .X.u = 3, .X.t = 2 };
	L4_Set_Propagation(&tag);
	L4_Set_VirtualSender(muidl_get_sender());
	L4_LoadMR(0, tag.raw);
	L4_LoadMR(1, 0xb00b);
	L4_LoadMR(2, dirfd);
	L4_LoadMR(3, flags);
	L4_StringItem_t rp = L4_StringItem(sizeof realpath, realpath);
	L4_LoadMRs(4, 2, rp.raw);
	tag = L4_Send(initrd_tid);
	if(L4_IpcFailed(tag)) {
		printf("%s: failed to propagate, ec=%#lx\n", __func__,
			L4_ErrorCode());
		return -EFAULT;	/* TODO: better error code? */
	} else {
		muidl_raise_no_reply();
		return 0;
	}
}


/* clear UTCB bit in root for this thread, because it won't have been covered
 * in add_systask()
 */
static void reserve_utcb_for_uapi(void)
{
	struct systask *root = get_systask(pidof_NP(L4_Myself()));
	int my_slot = ((L4_MyLocalId().raw & ~(L4_UtcbSize(the_kip) - 1))
		- L4_Address(root->task.utcb_area)) >> L4_UtcbAlignmentLog2(the_kip);
	assert(bitmap_test_bit(root->task.utcb_free, my_slot));
	bitmap_clear_bit(root->task.utcb_free, my_slot);
}


int uapi_loop(void *param_ptr)
{
	uapi_tid = L4_Myself();
	reserve_utcb_for_uapi();

	static const struct root_uapi_vtable vtab = {
		.create_thread = &uapi_create_thread,
		.remove_thread = &uapi_remove_thread,
		.spawn = &uapi_spawn,
		.kill = &root_uapi_kill,
		.exit = &uapi_exit,
		.wait = &uapi_wait,
		.fork = &uapi_fork,
		.sigconfig = &root_uapi_sigconfig,
		.sigset = &root_uapi_sigset,
		.sigsuspend = &root_uapi_sigsuspend,
		.getresugid = &uapi_getresugid,
		.setresugid = &uapi_setresugid,
#ifdef BUILD_SELFTEST
		.get_systask_threads = &uapi_get_systask_threads,
#endif

		/* Sneks::Path */
		.resolve = &uapi_resolve,
	};
	for(;;) {
		L4_Word_t st = _muidl_root_uapi_dispatch(&vtab);
		if(check_rollback(st)) continue;
		else if(st == MUIDL_UNKNOWN_LABEL) {
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


void uapi_init(void)
{
	utcb_size_log2 = size_to_shift(L4_UtcbSize(the_kip));
	assert(1 << utcb_size_log2 >= L4_UtcbSize(the_kip));
	ra_systask = RA_NEW(struct systask, NUM_SYSTASK_IDS);
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
