
/* userspace API portion of the root task. */

#define ROOTUAPI_IMPL_SOURCE

#include <stdio.h>
#include <errno.h>
#include <ccan/htable/htable.h>
#include <ccan/darray/darray.h>
#include <ccan/bitmap/bitmap.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <ukernel/util.h>
#include <ukernel/rangealloc.h>
#include <sneks/process.h>

#include "muidl.h"
#include "root-uapi-defs.h"
#include "defs.h"


#define MAX_PID 0xffff
#define IS_SYSTASK(pid) ((pid) >= SNEKS_MIN_SYSID)

#define TNOS_PER_BITMAP (1 << 16)


typedef darray(L4_ThreadId_t) tidlist;

struct task_base
{
	L4_Fpage_t kip_area, utcb_area;
	tidlist threads;
	bitmap *utcb_free;
};


struct u_task {
	struct task_base t;
};


struct systask {
	struct task_base t;
};


union task_all {
	struct task_base base;
	struct u_task u;
	struct systask sys;
};


static struct rangealloc *ra_tasks = NULL;
static int utcb_size_log2;
L4_ThreadId_t uapi_tid;

/* TID allocator. this is for the 32-bit mode where the two highest bits of
 * ThreadNo are the two highest bits of the process ID.
 */
static bitmap *tno_free_maps[4];
static int tno_free_counts[4];


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


int add_task(int pid, L4_Fpage_t kip, L4_Fpage_t utcb)
{
	if(pid <= 0 || pid >= MAX_PID) return -EINVAL;

	union task_all *ta = ra_alloc(ra_tasks, pid);
	if(ta == NULL) return -EEXIST;

	ta->base.kip_area = kip;
	ta->base.utcb_area = utcb;
	darray_init(ta->base.threads);
	ta->base.utcb_free = bitmap_alloc1(
		1 << (L4_SizeLog2(utcb) - utcb_size_log2));
	if(pid == SNEKS_MIN_SYSID) {
		/* roottask initialization. */
		darray_push(ta->base.threads, L4_MyGlobalId());
		bitmap_clear_bit(ta->base.utcb_free, 0);
	} else if(pid > SNEKS_MIN_SYSID && pid == pidof_NP(L4_Pager())) {
		/* sysmem init. */
		darray_push(ta->base.threads, L4_Pager());
		bitmap_clear_bit(ta->base.utcb_free, 0);
	}

	return 0;
}


L4_ThreadId_t allocate_thread(int pid, void **utcb_loc_p)
{
	assert(utcb_loc_p != NULL);

	int map = IS_SYSTASK(pid) ? 0 : (pid & 0x6000) >> 13;
	while(tno_free_counts[map] == 0 && IS_SYSTASK(pid) && map < 4) map++;
	if(map > 3 || tno_free_counts[map] == 0) return L4_nilthread;

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

	union task_all *ta = ra_id2ptr(ra_tasks, pid);
	assert(!L4_IsNilFpage(ta->base.utcb_area));

	int n_slots = 1 << (L4_SizeLog2(ta->base.utcb_area) - utcb_size_log2),
		utcb_slot = bitmap_ffs(ta->base.utcb_free, 0, n_slots);
	if(utcb_slot == n_slots) {
		bitmap_set_bit(tno_free_maps[map], bit);
		return L4_nilthread;
	}
	bitmap_clear_bit(ta->base.utcb_free, utcb_slot);
	darray_push(ta->base.threads, tid);

	*utcb_loc_p = (void *)(L4_Address(ta->base.utcb_area)
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


static bool task_remove_thread(
	union task_all *ta, L4_ThreadId_t tid, void *utcb_loc)
{
	tid = L4_GlobalIdOf(tid);
	if(!tidlist_remove_from(&ta->base.threads, tid)) return false;

	int map = L4_ThreadNo(tid) >> 16, bit = L4_ThreadNo(tid) & 0xffff;
	bitmap_set_bit(tno_free_maps[map], bit);
	tno_free_counts[map]++;
	assert(tno_free_counts[map] <= TNOS_PER_BITMAP);

	assert((L4_Word_t)utcb_loc >= L4_Address(ta->base.utcb_area));
	L4_Word_t pos = (L4_Word_t)utcb_loc - L4_Address(ta->base.utcb_area);
	assert(pos < L4_Size(ta->base.utcb_area));
	int u_slot = pos >> utcb_size_log2;
	assert(!bitmap_test_bit(ta->base.utcb_free, u_slot));
	bitmap_set_bit(ta->base.utcb_free, u_slot);

	return true;
}


void free_thread(L4_ThreadId_t tid, void *utcb_loc)
{
	assert(L4_IsGlobalId(tid));
	int pid = pidof_NP(tid);
	assert(pid > 0);
	union task_all *ta = ra_id2ptr(ra_tasks, pid);
	assert(!L4_IsNilFpage(ta->base.utcb_area));
	if(!task_remove_thread(ta, tid, utcb_loc)) {
		printf("%s: tid=%#lx not present in pid=%d\n", __func__, tid.raw, pid);
		abort();
	}
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

	union task_all *ta = ra_id2ptr(ra_tasks, pid);
	if(L4_IsNilFpage(ta->base.utcb_area)) return -EINVAL;

	bool first_thread = ta->base.threads.size == 0;
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
		int n = make_space(tid, ta->base.kip_area, ta->base.utcb_area);
		if(n < 0) {
			/* FIXME: free_thread() will kill the task here, because its
			 * thread count drops to zero. probably this is not what's wanted.
			 */
			free_thread(tid, utcb_loc);
			return n;
		}
	}

	/* TODO: use vm_tid for non-systask paging. */
	L4_ThreadId_t space = ta->base.threads.item[0],
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
	union task_all *ta = ra_id2ptr(ra_tasks, pid);
	if(L4_IsNilFpage(ta->base.utcb_area)) return -EINVAL;
	if(!task_remove_thread(ta, tid, (void *)utcb_addr)) return -EINVAL;

	L4_Word_t res = L4_ThreadControl(tid, L4_nilthread, L4_nilthread,
		L4_nilthread, (void *)-1);
	if(res != 1) {
		printf("uapi: deleting ThreadControl failed, res=%lu\n", res);
		abort();
	}

	if(ta->base.threads.size == 0) {
		printf("uapi: would delete pid=%d!\n", pid);
		/* TODO: zombify or wait(2) `pid'. */
	}

	if(is_self) muidl_raise_no_reply();
	return 0;
}


static int uapi_spawn(
	const char *filename, const char *args, const char *envs)
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
	ra_tasks = RA_NEW(union task_all, 1 << 16);
	ra_disable_id_0(ra_tasks);

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
