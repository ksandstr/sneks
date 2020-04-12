
/* systests for lifecycle notification. */
#define __SNEKS__ 1

#include <stdbool.h>
#include <errno.h>
#include <assert.h>
#include <ccan/array_size/array_size.h>
#include <l4/types.h>
#include <l4/ipc.h>

#include <sneks/msg.h>
#include <sneks/process.h>
#include <sneks/test.h>


struct lcstate {
	L4_ThreadId_t parent;
	int n_fork, n_exec, n_exit;
	int children[16], execs[16], exits[16];
};


static bool lcstate_handler_fn(
	int bit, L4_Word_t *body, int length, void *priv)
{
	struct lcstate *st = priv;
	switch(body[1] & 0xff) {
		case MPL_FORK:
			st->children[st->n_fork++ % ARRAY_SIZE(st->children)] = body[1] >> 8;
			break;
		case MPL_EXEC:
			st->execs[st->n_exec++ % ARRAY_SIZE(st->execs)] = body[0];
			break;
		case MPL_EXIT:
			st->exits[st->n_exit++ % ARRAY_SIZE(st->exits)] = body[0];
			break;
		default:
			diag("%s: weird body[0]=%#lx", __func__, body[0]);
	}

	L4_Accept(L4_UntypedWordsAcceptor);
	L4_LoadMR(0, 0);
	L4_MsgTag_t tag = L4_Lcall(st->parent);
	if(L4_IpcFailed(tag)) {
		diag("%s: parent call failed, ec=%lu", __func__, L4_ErrorCode());
	}

	return true;
}


START_TEST(notify_fork_exit)
{
	plan_tests(4);

	struct lcstate *st = malloc(sizeof *st);
	*st = (struct lcstate){ .parent = L4_MyLocalId() };
	int msgh = sysmsg_listen(MSGB_PROCESS_LIFECYCLE, &lcstate_handler_fn, st);
	if(msgh < 0) diag("sysmsg_listen() failed, errno=%d", errno);
	assert(msgh >= 0);

	char *args[] = { "initrd_spawn_partner", "fork-and-exit", NULL },
		*envs[] = { NULL };
	int partner = spawn_NP("/initrd/systest/sys/test/initrd_spawn_partner",
		args, envs);
	skip_start(!ok(partner > 0, "partner spawned"), 3,
		"spawn_NP() failed, errno=%d", errno)
	{
		L4_MsgTag_t tag;
		do {
			L4_ThreadId_t sender;
			L4_Accept(L4_UntypedWordsAcceptor);
			tag = L4_Wait_Timeout(L4_TimePeriod(7000), &sender);
			if(L4_IpcSucceeded(tag)) {
				L4_LoadMR(0, 0);
				L4_Reply(sender);
			}
		} while(L4_IpcSucceeded(tag) || L4_ErrorCode() != 3);
		/* close the listener to ensure safe access of `st' */
		int n = sysmsg_close(msgh);
		if(n != 0) diag("sysmsg_close() failed, n=%d", n);
		msgh = -1;
		diag("loop completed (f=%d, x=%d, e=%d)",
			st->n_fork, st->n_exec, st->n_exit);

		ok1(st->n_fork == 1);
		ok1(st->n_exit == 2);
		ok((st->exits[0] == st->children[0] && st->exits[1] == partner)
			|| (st->exits[1] == st->children[0] && st->exits[0] == partner),
			"exiting PIDs correct");
	} skip_end;

	if(msgh >= 0) {
		int n = sysmsg_close(msgh);
		if(n != 0) diag("sysmsg_close() failed, n=%d", n);
	}
	free(st);
}
END_TEST

SYSTEST("uapi:lifecycle", notify_fork_exit);
