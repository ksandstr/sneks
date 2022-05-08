/* sysmsg client-side for systasks. */
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <threads.h>
#include <ccan/darray/darray.h>
#include <ccan/array_size/array_size.h>
#include <ccan/compiler/compiler.h>
#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>
#include <sneks/systask.h>
#include <sneks/msg.h>
#include <sneks/sys/msg-defs.h>
#include <sneks/sys/info-defs.h>

struct handler {
	sysmsg_handler_fn fn;
	void *priv;
};

static int receiver_fn(void *);

static L4_ThreadId_t sysmsg_tid = { .raw = 0 };
static once_flag init_flag = ONCE_FLAG_INIT;
static thrd_t receiver;
static tss_t current_handler;
static darray(struct handler) handlers[32];

static COLD void initialize(void)
{
	int n = tss_create(&current_handler, NULL);
	if(n != thrd_success) {
		log_crit("tss_create failed, n=%d", n);
		abort();
	}
	for(int i=0; i < ARRAY_SIZE(handlers); i++) darray_init(handlers[i]);

	L4_ThreadId_t sysinfo_tid;
	n = __info_lookup(L4_Pager(), &sysinfo_tid.raw);
	if(n != 0) {
		log_crit("__info_lookup failed, n=%d", n);
		abort();
	}
	struct sneks_sysapi_info info;
	n = __info_sysapi_block(sysinfo_tid, &info);
	if(n != 0) {
		log_crit("__info_sysapi_block failed, n=%d", n);
		abort();
	}
	sysmsg_tid.raw = info.sysmsg;

	n = thrd_create(&receiver, &receiver_fn, NULL);
	if(n != thrd_success) {
		log_crit("thrd_create() failed, n=%d", n);
		abort();
	}
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_LoadMR(0, 0);
	L4_MsgTag_t tag = L4_Lcall(tidof_NP(receiver));
	if(L4_IpcFailed(tag)) {
		log_crit("can't sync with receiver, ec=%lu", L4_ErrorCode());
		abort();
	}
}

static L4_Word_t invoke_handlers(int bits, L4_Word_t body[static 62], int length)
{
	bool immediate = true;
	while(bits > 0) {
		int b = ffsl(bits) - 1;
		bits &= ~(1 << b);
		/* TODO: check deferral here, queue the message up where applicable */
		for(int i=0; i < handlers[b].size; i++) {
			struct handler *h = &handlers[b].item[i];
			tss_set(current_handler, h);
			immediate &= (*h->fn)(b, body, length, h->priv);
		}
	}
	tss_set(current_handler, NULL);
	return immediate ? 0 : 1;
}

static int receiver_fn(void *param_ptr)
{
	int32_t oldmask = 0;
	int n = __sysmsg_setmask(sysmsg_tid, &oldmask, 0, ~0u);
	if(n != 0) {
		log_err("Sysmsg::setmask failed, n=%d", n);
		return -1;
	}

	L4_ThreadId_t from;
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_MsgTag_t tag = L4_WaitLocal_Timeout(L4_Never, &from);
	if(L4_IpcFailed(tag)) return L4_ErrorCode();
	assert(L4_IsLocalId(from));
	L4_LoadMR(0, 0);
	L4_Reply(from);

	for(;;) {
		L4_Accept(L4_UntypedWordsAcceptor);
		L4_MsgTag_t tag = L4_Receive(sysmsg_tid);
		for(;;) {
			L4_ThreadId_t actual = L4_IpcPropagated(tag) ? L4_ActualSender() : sysmsg_tid;
			if(L4_IpcFailed(tag)) {
				log_err("ipc with sysmsg failed: ec=%lu", L4_ErrorCode());
				break;
			}
			if(L4_Label(tag) != 0xe807 || L4_TypedWords(tag) > 0
				|| L4_UntypedWords(tag) < 1)
			{
				log_info("weird tag=%#lx from sysmsg", tag.raw);
				break;
			}

			L4_Word_t bits, body[62];
			L4_StoreMR(1, &bits);
			int length = L4_UntypedWords(tag) - 1;
			assert(length <= ARRAY_SIZE(body));
			L4_StoreMRs(2, length, body);
			L4_Word_t st = invoke_handlers(bits, body, length);

			L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1 }.raw);
			L4_LoadMR(1, st);
			/* "ReplyReceive" */
			L4_ThreadId_t dummy;
			L4_Accept(L4_UntypedWordsAcceptor);
			tag = L4_Ipc(actual, sysmsg_tid,
				L4_Timeouts(L4_ZeroTime, L4_Never), &dummy);
		}
	}
	return 0;
}

int sysmsg_listen(int bit, sysmsg_handler_fn fn, void *priv)
{
	assert(bit >= 0 && bit < ARRAY_SIZE(handlers));
	call_once(&init_flag, &initialize);
	if(handlers[bit].size == 0) {
		int32_t oldmask = 0;
		int n = __sysmsg_setmask(sysmsg_tid, &oldmask, 1u << bit, ~0u);
		if(n < 0) return n; else if(n > 0) return -EIO;
	}
	darray_push(handlers[bit], (struct handler){ .fn = fn, .priv = priv });
	return (bit & (ARRAY_SIZE(handlers) - 1)) | (handlers[bit].size - 1) << 6;
}

int sysmsg_add_filter(int handle, const L4_Word_t *labels, int n_labels)
{
	if(n_labels < 1) return 0;
	if(handle < 0) return -EBADF;
	int bit = handle % ARRAY_SIZE(handlers), offs = handle >> 6;
	assert(offs < handlers[bit].size);
	return __sysmsg_add_filter(sysmsg_tid, 1u << bit, labels, n_labels);
}

int sysmsg_rm_filter(int handle, const L4_Word_t *labels, int n_labels)
{
	if(n_labels < 1) return 0;
	if(handle < 0) return -EBADF;
	int bit = handle % ARRAY_SIZE(handlers), offs = handle >> 6;
	assert(offs < handlers[bit].size);
	return __sysmsg_rm_filter(sysmsg_tid, 1u << bit, labels, n_labels);
}

int sysmsg_broadcast(int maskp, int maskn, const L4_Word_t *body, int length)
{
	call_once(&init_flag, &initialize);
	if(tss_get(current_handler) != NULL) return -EDEADLK;
	bool immediate;
	int n = __sysmsg_broadcast(sysmsg_tid, &immediate, maskp, maskn, body, length);
	return n != 0 ? n : (immediate ? 0 : 1);
}

int sysmsg_close(int handle)
{
	if(init_flag == ONCE_FLAG_INIT) return -EBADF;
	if(handle < 0) return -EBADF;
	int bit = handle % ARRAY_SIZE(handlers), pos = handle >> 6;
	if(handlers[bit].size <= pos) return -EBADF;

	assert(handlers[bit].item[pos].fn != NULL);
	handlers[bit].item[pos] = (struct handler){ .fn = NULL };
	/* WIBNI there were a darray_last()? */
	while(handlers[bit].size > 0
		&& handlers[bit].item[handlers[bit].size - 1].fn == NULL)
	{
		darray_resize(handlers[bit], handlers[bit].size - 1);
	}
	if(handlers[bit].size == 0) {
		int32_t oldmask;
		int n = __sysmsg_setmask(sysmsg_tid, &oldmask, 0, ~(1ul << bit));
		if(n != 0) return n;
	}
	return 0;
}
