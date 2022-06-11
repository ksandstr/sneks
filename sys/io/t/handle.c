#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <errno.h>
#include <fcntl.h>
#include <l4/types.h>
#include <l4/ipc.h>
#include <l4/thread.h>
#include <sneks/api/io-defs.h>
#include <sneks/systask.h>
#include <sneks/test.h>

static const char *testfile = "/initrd/systest/sys/test/initrd_spawn_partner";

struct dup_param {
	L4_ThreadId_t server;
	int handle;
};

static int partner_fn(void *priv)
{
	struct dup_param *p = priv;
	int rc = __io_touch(p->server, p->handle);
	__io_close(p->server, p->handle);
	free(p);
	return rc;
}

START_LOOP_TEST(dup_to_wildcard, iter, 0, 3)
{
	const bool wildcard = iter & 1, systask = iter & 2;
	diag("wildcard=%s, systask=%s", btos(wildcard), btos(systask));
	plan_tests(5);
	int fd = open(testfile, O_RDONLY);
	fail_if(fd < 0, "open testfile: %s", strerror(errno));
	int copy, n = __io_dup_to(fserv(fd), &copy, fhand(fd), wildcard ? 0x10000 : SNEKS_MIN_SYSID);
	skip_start(!ok(n == 0, "IO/dup"), 4, "failed: %s", stripcerr(n)) {
		L4_Word_t st;
		if(!systask) {
			char server[32], handle[32], self[32];
			snprintf(server, sizeof server, "SERVER=%#lx", fserv(fd).raw);
			snprintf(handle, sizeof handle, "HANDLE=%lu", (unsigned long)copy);
			snprintf(self, sizeof self, "RETURN=%#lx", L4_MyGlobalId().raw);
			char *argv[] = { "initrd_spawn_partner", "dup-to-wildcard", NULL }, *envp[] = { server, handle, self, NULL };
			int cpid = spawn_NP("/initrd/systest/sys/test/initrd_spawn_partner", argv, envp);
			if(!ok(cpid > 0, "spawn")) diag("spawn failed: %s", strerror(errno));
			skip(1, "userspace partner");
			L4_ThreadId_t sender;
			L4_MsgTag_t tag;
			do tag = L4_Wait(&sender); while(L4_IpcSucceeded(tag) && pidof_NP(sender) != cpid);
			if(L4_IpcFailed(tag)) st = L4_ErrorCode(); else L4_StoreMR(1, &st);
		} else {
			skip(1, "systask thread partner");
			struct dup_param *param = malloc(sizeof *param); fail_if(param == NULL);
			*param = (struct dup_param){ .server = fserv(fd), .handle = copy };
			thrd_t oth; ok(thrd_create(&oth, &partner_fn, param) == thrd_success, "thrd_create");
			int tmp; thrd_join(oth, &tmp); st = tmp;
		}
		if(st != 0) diag("partner status: %s", stripcerr(st));
		iff_ok1(systask && wildcard, st == 0);
		imply_ok1(!wildcard || !systask, st == -EBADF);
	} skip_end;
	__io_close(fserv(fd), copy);
	close(fd);
}
END_TEST

SYSTEST("io:handle", dup_to_wildcard);
