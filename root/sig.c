
#define ROOTUAPI_IMPL_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <sys/types.h>

#include <l4/types.h>
#include <l4/ipc.h>
#include <l4/thread.h>
#include <ukernel/rangealloc.h>
#include <sneks/mm.h>
#include <sneks/process.h>
#include <sneks/api/vm-defs.h>

struct sneks_io_statbuf; /* foo */
struct sneks_proc_rlimit; /* foo2 */
#include "muidl.h"
#include "root-impl-defs.h"
#include "defs.h"


static const uint8_t sigpage_tail_code[] = {
/* tophalf: */
	0xe8, 0x08, 0x00, 0x00, 0x00,	/* call d <get_vec> */
/* 1: */
	0xff, 0x93, 0x0c, 0x00, 0x00, 0x00,	/* call *(botvec - .)(%ebx) */
	0xeb, 0xf8,				/* jmp 1b */
/* get_vec: */
	0x8b, 0x1c, 0x24,		/* movl (%esp), %ebx */
	0xc3,					/* ret */
/* botvec: */
	0x00, 0x00, 0x00, 0x00,		/* (bottomhalf vector) */
};


static void sig_deliver(struct process *p, int sig, bool self)
{
	assert(sig >= 1 && sig <= 64);

	/* TODO: consider launching a helper-redirecting threadlet when
	 * sighelper_tid is nonzero, so that running signal handlers can be
	 * interrupted to run other handlers for reasons besides self-signaling or
	 * block mask clearing.
	 */
	if(!L4_IsNilThread(p->sighelper_tid) || self) goto add_sig;

	/* create the intermediary thread. TODO: move this into a
	 * spawn_threadlet() or some such.
	 */
	assert(p->task.threads.size > 0);
	p->sighelper_tid = allocate_thread(ra_ptr2id(ra_process, p),
		&p->sighelper_utcb);
	if(L4_IsNilThread(p->sighelper_tid)) {
		printf("uapi:%s: can't allocate thread ID when sig=%d\n",
			__func__, sig);
		return;
	}
	L4_ThreadId_t space = p->task.threads.item[0];
	L4_Word_t res = L4_ThreadControl(p->sighelper_tid, space,
		L4_Myself(), vm_tid, p->sighelper_utcb);
	if(res != 1) {
		printf("uapi:%s: ThreadControl failed, ec=%lu\n",
			__func__, L4_ErrorCode());
		abort();	/* FIXME: do something else! */
	}

	L4_Word_t ip = p->sigpage_addr + PAGE_SIZE - sizeof sigpage_tail_code,
		sp = (ip - 64) & ~63, rc;
	int n = __vm_breath_of_life(vm_tid, &rc, p->sighelper_tid.raw, sp, ip);
	if(n != 0) {
		printf("uapi:%s: VM::breath_of_life failed, n=%d\n", __func__, n);
		abort();	/* FIXME: unfuckinate! */
	}

add_sig:
	p->pending_set |= (1ull << (sig - 1));
}


static bool sig_default(struct process *p, int sig)
{
	switch(sig) {
		default:
			/* FIXME: add the rest, assert against this */
			printf("%s: ignoring sig=%d by default (perhaps wrongly)\n", __func__, sig);
			/* FALL THRU */

		/* ignored signals. */
		case SIGCHLD:
			assert(~p->pending_set & (1ull << sig));
			return false;

		/* signals that cause coredump. */
		case SIGSEGV:
		case SIGABRT:
			/* FALL THRU */
		/* signals that cause process termination. */
		case SIGKILL:
		case SIGTERM:
			/* death. (would also dump core.) */
			p->code = CLD_KILLED;
			p->signo = sig;
			p->status = 0;
			zombify(p);
			return true;
	}
}


/* TODO: add varargs for signal-specific parameters passed down to
 * sig_deliver? though these won't be saved in the pending set unless there
 * were fields in <struct process> for each, which seems ugly and wouldn't
 * play well with how Proc::sigset is defined.
 */
void sig_send(struct process *p, int sig, bool self)
{
	assert(sig >= 1 && sig <= 64);
	uint64_t sig_bit = 1ull << (sig - 1);
	if((p->mask_set & sig_bit) != 0) p->pending_set |= sig_bit;
	else if(~p->pending_set & sig_bit) {
		if((p->dfl_set & sig_bit) != 0) {
			assert((p->ign_set & sig_bit) == 0);
			if(sig_default(p, sig) && self) {
				muidl_raise_no_reply();
			}
		} else if((p->ign_set & sig_bit) == 0) {
			sig_deliver(p, sig, self);
		}
	}
}


int root_uapi_kill(pid_t pid, int sig)
{
	struct process *p = get_process(pid);
	if(p == NULL) return -ESRCH;

	const int sender_pid = pidof_NP(muidl_get_sender());
	struct process *sender = get_process(sender_pid);
	/* TODO: also permit SIGCONT within session. */
	if(!IS_SYSTASK(sender_pid)
		&& sender->eff_uid != 0
		&& sender->real_uid != p->real_uid
		&& sender->real_uid != p->saved_uid
		&& sender->eff_uid != p->real_uid
		&& sender->eff_uid != p->saved_uid)
	{
		return -EPERM;
	}

	if(sig < 0 || sig > 64) return -EINVAL;
	if(sender_pid == pidof_NP(vm_tid)) {
		/* reply vm's calls to Proc::kill early, because zombify() in
		 * sig_send()'s call tree tries to do VM::erase, which cornflakes
		 * things the fuck out.
		 */
		L4_LoadMR(0, 0);
		L4_MsgTag_t tag = L4_Reply(muidl_get_sender());
		if(L4_IpcSucceeded(tag)) muidl_raise_no_reply();
		else {
			printf("%s: early reply to vm failed, ec=%lu\n",
				__func__, L4_ErrorCode());
			return -EAGAIN;	/* try pie, try. */
		}
	}
	if(sig != 0) sig_send(p, sig, pid == sender_pid);
	return 0;
}


void root_uapi_sigconfig(
	L4_Word_t sigpage_addr,
	uint8_t tail_data[static 1024], unsigned *tail_data_len,
	int *handler_offset_p)
{
	memset(tail_data, '\0', 1024);
	*tail_data_len = 1024;
	*handler_offset_p = 1020;

	int pid = pidof_NP(muidl_get_sender());
	if(IS_SYSTASK(pid)) return;	/* nuh! */

	struct process *self = get_process(pid);
	self->sigpage_addr = sigpage_addr;
	size_t len = sizeof sigpage_tail_code;
	memset(tail_data, '\0', *tail_data_len - len);
	memcpy(&tail_data[*tail_data_len - len], sigpage_tail_code, len);
	*handler_offset_p = PAGE_SIZE - 4;
}


/* TODO: handle the case where mask_set would be altered while PF_SAVED_MASK
 * is set.
 */
uint64_t root_uapi_sigset(int set_name, uint64_t or_bits, uint64_t and_bits)
{
	struct process *p = get_process(pidof_NP(muidl_get_sender()));
	if(set_name == 4 && (p->pending_set | and_bits) == 0
		&& !L4_IsNilThread(p->sighelper_tid)
		&& L4_SameThreads(muidl_get_sender(), p->sighelper_tid))
	{
		/* terminating condition for the helper thread. */
		sig_remove_helper(p);
		muidl_raise_no_reply();
		return 0;
	}

	uint64_t *set;
	bool conceal = false;
	switch(set_name) {
		case 0: set = &p->ign_set; break;
		case 1: set = &p->dfl_set; break;
		case 2: set = &p->mask_set; break;
		case 4:
			conceal = true;
			/* FALL THRU */
		case 3:
			set = &p->pending_set;
			break;
		default:
			fprintf(stderr, "%s: unknown set_name=%d from pid=%d\n",
				__func__, set_name, ra_ptr2id(ra_process, p));
			return 0;
	}

	uint64_t oldval = *set;
	if(!conceal) {
		*set &= and_bits;
	} else {
		oldval &= ~p->mask_set;
		*set &= and_bits | p->mask_set;
	}
	if(set != &p->pending_set) {
		/* modification, not permitted for pending_set. */
		*set |= or_bits;
		uint64_t pos_change = oldval ^ *set;
		if(pos_change != 0) {
			if(set == &p->ign_set) p->dfl_set &= ~pos_change;
			else if(set == &p->dfl_set) p->ign_set &= ~pos_change;
		}
		assert((p->ign_set & p->dfl_set) == 0);
	} else if(oldval != 0 && (p->flags & PF_SAVED_MASK)) {
		/* put all but first bit back into pending_set. */
		int low = ffsll(oldval) - 1;
		assert(low >= 0);
		p->pending_set |= oldval & ~(1ull << low);
		oldval = 1ull << low;
		assert(ffsl(oldval) - 1 == low);
		/* turn sigsuspend() off. */
		p->flags &= ~PF_SAVED_MASK;
		p->mask_set = p->saved_mask_set;
	}
	if(set == &p->mask_set) {
		/* trigger pending signals that were just unmasked, filtering
		 * thru possible default and ignore behaviour.
		 */
		uint64_t trig = p->pending_set & (oldval & ~and_bits);
		p->pending_set &= ~trig;
		trig &= ~p->ign_set;
		while(trig != 0) {
			int sig = ffsll(trig);
			/* caller invokes masked-pending handlers synchronously, as though
			 * sent from kill(getpid(), _ <- trig).
			 */
			sig_send(p, sig, true);
			trig &= ~(1ull << (sig - 1));
		}
	}

	return oldval;
}


int root_uapi_sigsuspend(uint64_t mask)
{
	struct process *p = get_process(pidof_NP(muidl_get_sender()));
	if(p == NULL) return -EINVAL;

	if(p->flags & PF_SAVED_MASK) {
		/* TODO: change this to a WARN() or some such, perhaps dependent on
		 * a per-process debug setting or something.
		 */
		printf("%s: pid=%u already has ongoing sigsuspend()?\n", __func__,
			ra_ptr2id(ra_process, p));
		return -EAGAIN;
	}

	/* trigger at most one signal out of the pending set immediately. polly
	 * wanna cracker.
	 */
	int trig = ffsll(p->pending_set & ~mask);
	if(trig > 0) {
		p->pending_set &= ~(1 << (trig - 1));
		return trig;
	}

	/* change mode for uapi_sigset(). */
	p->saved_mask_set = p->mask_set;
	p->mask_set = mask;
	assert((p->pending_set & ~p->mask_set) == 0ull);
	p->flags |= PF_SAVED_MASK;
	muidl_raise_no_reply();
	return 0;
}
