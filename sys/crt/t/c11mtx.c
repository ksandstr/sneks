/* basic tests on the C11 mtx_*() interfaces. many of these tests were adapted
 * near-verbatim from self_suite in mung's testbench; copypasta warnings
 * apply.
 */
#include <threads.h>
#include <l4/types.h>
#include <l4/ipc.h>
#include <sneks/test.h>

#define QUIT_LABEL 0xdead

START_TEST(init_plain_mutex)
{
	plan_tests(1);

	mtx_t *mtx = malloc(sizeof(mtx_t));
	fail_if(mtx == NULL);
	int n = mtx_init(mtx, mtx_plain);
	ok(n == thrd_success, "mtx_init returned success");

	mtx_destroy(mtx);
	free((void *)mtx);
}
END_TEST


START_TEST(trylock_plain_mutex)
{
	plan_tests(3);

	mtx_t *mtx = malloc(sizeof(mtx_t));
	fail_if(mtx == NULL);
	int n = mtx_init(mtx, mtx_plain);
	fail_if(n != thrd_success);

	n = mtx_trylock(mtx);
	ok(n == thrd_success, "mtx_trylock returned success");
	n = mtx_trylock(mtx);
	ok(n == thrd_busy, "mtx_trylock returned busy");
	mtx_destroy(mtx);
	pass("mtx_destroy didn't abort");

	free((void *)mtx);
}
END_TEST


START_TEST(lock_plain_mutex)
{
	plan_tests(2);

	mtx_t *mtx = malloc(sizeof(mtx_t));
	fail_if(mtx == NULL);
	int n = mtx_init(mtx, mtx_plain);
	fail_if(n != thrd_success);

	n = mtx_lock(mtx);
	if(!ok(n == thrd_success, "mtx_lock returned success")) {
		diag("n=%d", n);
	}
	n = mtx_unlock(mtx);
	if(!ok(n == thrd_success, "mtx_unlock returned success")) {
		diag("n=%d", n);
	}

	mtx_destroy(mtx);
	free((void *)mtx);
}
END_TEST


static int lock_and_hold_fn(void *param_ptr)
{
	mtx_t *mtx = param_ptr;
	int n = mtx_lock(mtx);
	fail_unless(n == thrd_success);

	L4_Accept(L4_UntypedWordsAcceptor);
	L4_ThreadId_t sender;
	L4_MsgTag_t tag = L4_Wait(&sender);
	if(L4_IpcFailed(tag)) {
		diag("%s: ipc failed, ec=%#lx", __func__, L4_ErrorCode());
	} else if(L4_Label(tag) != QUIT_LABEL) {
		diag("%s: weird tag=%#lx", __func__, tag.raw);
	}

	return mtx_unlock(mtx);
}


static bool send_quit(L4_ThreadId_t dest)
{
	L4_LoadMR(0, (L4_MsgTag_t){ .X.label = QUIT_LABEL }.raw);
	L4_MsgTag_t tag = L4_Send_Timeout(dest, L4_TimePeriod(2000));
	return L4_IpcSucceeded(tag);
}


/* variables:
 *   - [do_trylock] whether the main thread locks the mutex using
 *     mtx_trylock(), or mtx_lock()
 */
START_LOOP_TEST(conflict_plain_mutex, iter, 0, 1)
{
	bool do_trylock = !!(iter & 1);
	diag("do_trylock=%s", btos(do_trylock));
	plan_tests(7);

	mtx_t *mtx = malloc(sizeof *mtx);
	mtx_init(mtx, mtx_plain);
	int n;
	if(do_trylock) n = mtx_trylock(mtx); else n = mtx_lock(mtx);
	if(!ok(n == thrd_success, "own lock succeeds")) {
		diag("n=%d", (int)n);
	}

	thrd_t oth_thrd;
	n = thrd_create(&oth_thrd, &lock_and_hold_fn, (void *)mtx);
	fail_unless(n == thrd_success);
	L4_ThreadId_t oth = tidof_NP(oth_thrd);
	bool quit_ok = send_quit(oth);
	ok(!quit_ok, "other thread blocks");

	n = mtx_unlock(mtx);
	if(!ok(n == thrd_success, "own unlock succeeds")) {
		diag("n=%d", (int)n);
	}
	L4_ThreadSwitch(oth);
	n = mtx_trylock(mtx);
	ok(n == thrd_busy, "other thread owns lock");
	send_quit(oth);
	L4_ThreadSwitch(oth);
	n = mtx_trylock(mtx);
	ok(n == thrd_success, "trylock succeeded after quit");

	n = mtx_unlock(mtx);
	if(!ok(n == thrd_success, "own unlock succeeds (after trylock)")) {
		diag("n=%d", (int)n);
	}

	int ret;
	n = thrd_join(oth_thrd, &ret);
	ok(ret == thrd_success, "other thread unlock succeeded");

	mtx_destroy(mtx);
	free((void *)mtx);
}
END_TEST


SYSTEST("crt:mtx", init_plain_mutex);
SYSTEST("crt:mtx", trylock_plain_mutex);
SYSTEST("crt:mtx", lock_plain_mutex);
SYSTEST("crt:mtx", conflict_plain_mutex);
