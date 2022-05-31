#include <stdlib.h>
#include <stdbool.h>
#include <threads.h>
#include <ccan/likely/likely.h>
#include <l4/types.h>
#include <sneks/rollback.h>
#include "muidl.h"

/* may track both rollback and confirm at once */
struct rbctx {
	L4_ThreadId_t sender;
	L4_MsgTag_t tag;
	struct {
		rollback_fn_t fn;
		L4_Word_t param;
		void *priv;
	} r, c;
};

static tss_t rollback_tss;
static once_flag init_flag = ONCE_FLAG_INIT;

static void initialize(void) {
	if(tss_create(&rollback_tss, &free) != thrd_success) abort();
}

static struct rbctx *get_ctx(void)
{
	call_once(&init_flag, &initialize);
	struct rbctx *ctx = tss_get(rollback_tss);
	if(unlikely(ctx == NULL)) {
		if(ctx = malloc(sizeof *ctx), ctx == NULL) abort();
		*ctx = (struct rbctx){ };
		tss_set(rollback_tss, ctx);
	}
	return ctx;
}

void _set_rollback(rollback_fn_t fn, L4_Word_t param, const void *priv) {
	struct rbctx *ctx = get_ctx();
	ctx->r.fn = fn; ctx->r.param = param; ctx->r.priv = (void *)priv;
	ctx->sender = muidl_get_sender();
	ctx->tag = muidl_get_tag();
}

void _set_confirm(rollback_fn_t fn, L4_Word_t param, const void *priv) {
	struct rbctx *ctx = get_ctx();
	ctx->c.fn = fn; ctx->c.param = param; ctx->c.priv = (void *)priv;
	ctx->sender = muidl_get_sender();
	ctx->tag = muidl_get_tag();
}

void sync_confirm(void)
{
	call_once(&init_flag, &initialize);
	struct rbctx *ctx = tss_get(rollback_tss);
	if(likely(ctx != NULL) && ctx->c.fn != NULL) {
		rollback_fn_t fn = ctx->c.fn;
		ctx->c.fn = NULL;
		(*fn)(ctx->c.param, ctx->c.priv);
	}
}

bool check_rollback(L4_Word_t status)
{
	if(!MUIDL_IS_L4_ERROR(status)) return false;
	call_once(&init_flag, &initialize);
	bool yep = false;
	struct rbctx *ctx = tss_get(rollback_tss);
	if(status & 1) {
		/* receive-side fail implies that a previous reply succeeded; confirm
		 * its side effects eagerly, and explicitly cross off any rollback.
		 */
		sync_confirm();
		if(likely(ctx != NULL)) ctx->r.fn = NULL;
	} else if(likely(ctx != NULL) && ctx->tag.raw == muidl_get_tag().raw && ctx->sender.raw == muidl_get_sender().raw) {
		/* pop it, where available */
		if(ctx->r.fn != NULL) {
			rollback_fn_t fn = ctx->r.fn;
			ctx->r.fn = NULL;
			(*fn)(ctx->r.param, ctx->r.priv);
			yep = true;
		}
		ctx->c.fn = NULL; /* also clear the confirm handler */
	}
	if(ctx != NULL) {
		/* clear the rollback when it wasn't matched; that means the reply
		 * actually associated with the rollback did succeed.
		 */
		ctx->r.fn = NULL;
	}
	return yep;
}
