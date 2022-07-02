#include <l4/types.h>
#include <l4/thread.h>
#include <l4/syscall.h>
#include <l4/kip.h>
#include <sneks/thrd.h>
#include <sneks/api/proc-defs.h>
#include "defs.h"

int next_early_utcb_slot = 1;
const int __thrd_stksize_log2 = 14; /* NOTE: keep in sync with root/crt0-*.S! */

int __thrd_new(L4_ThreadId_t *tid)
{
	if(!L4_IsNilThread(uapi_tid)) return __proc_create_thread(uapi_tid, &tid->raw);
	else { /* forge the first anvil */
		static L4_Word_t utcb_base, next_tid = 0;
		if(next_tid == 0) {
			utcb_base = L4_MyLocalId().raw & ~((1u << L4_UtcbAlignmentLog2(the_kip)) - 1);
			next_tid = L4_ThreadNo(L4_Myself()) + 1;
		}
		/* use the forbidden range before UAPI for boot-up pagers etc. */
		*tid = L4_GlobalId(next_tid, L4_Version(L4_Myself()));
		void *utcb = (void *)utcb_base + next_early_utcb_slot * L4_UtcbSize(the_kip);
		if(L4_ThreadControl(*tid, L4_Myself(), L4_Myself(), L4_Pager(), utcb) == 0) return L4_ErrorCode();
		else {
			next_early_utcb_slot++; next_tid++;
			return 0;
		}
	}
}

int __thrd_destroy(L4_ThreadId_t tid) {
	if(!L4_IsNilThread(uapi_tid)) return __proc_remove_thread(uapi_tid, tid.raw, L4_LocalIdOf(tid).raw);
	else return L4_ThreadControl(tid, L4_nilthread, L4_nilthread, L4_nilthread, (void *)-1) == 1 ? 0 : L4_ErrorCode();
}

#ifdef BUILD_SELFTEST
#include <threads.h>
#include <sneks/test.h>
#include <sneks/systask.h>

static int return_one_fn(void *param_ptr) { return 1; }

START_TEST(start_and_join)
{
	diag("uapi_tid=%lu:%lu", L4_ThreadNo(uapi_tid), L4_Version(uapi_tid));
	plan(14 + 1);
	/* basic create and join, seven times over. */
	int total = 0;
	for(int i=0; i < 7; i++) {
		thrd_t t;
		int n = thrd_create(&t, &return_one_fn, NULL);
		ok(n == thrd_success, "created thread i=%d", i);
		int res = -1; n = thrd_join(t, &res);
		ok(n == thrd_success, "joined thread i=%d", i);
		total += res;
	}
	ok1(total == 7);
}
END_TEST

SYSTASK_SELFTEST("root:thrd", start_and_join);
#endif
