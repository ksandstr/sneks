
/* implementation of <sneks/rollback.h> for systasks. */

#include <stdlib.h>
#include <stdbool.h>
#include <threads.h>
#include <assert.h>
#include <ccan/likely/likely.h>
#include <l4/types.h>
#include <sneks/rollback.h>

#include "muidl.h"


/* we may be tracking both a confirm and a rollback at the same time, so
 * this is a bit more complicated than it obviously need be.
 */
struct rbctx
{
	L4_ThreadId_t sender;
	L4_MsgTag_t tag;
	struct {
		rollback_fn_t fn;
		L4_Word_t param;
		void *priv;
	} r, c;
};


static once_flag init_flag = ONCE_FLAG_INIT;
static tss_t rollback_tss;


static void initialize(void) {
	tss_create(&rollback_tss, &free);
}


static struct rbctx *get_ctx(void)
{
	call_once(&init_flag, &initialize);
	struct rbctx *ctx = tss_get(rollback_tss);
	if(unlikely(ctx == NULL)) {
		ctx = malloc(sizeof *ctx);
		assert(ctx != NULL);	/* fuck it! */
		*ctx = (struct rbctx){ };
		tss_set(rollback_tss, ctx);
	}
	return ctx;
}


void _set_rollback(rollback_fn_t fn, L4_Word_t param, const void *priv)
{
	struct rbctx *ctx = get_ctx();
	ctx->r.fn = fn; ctx->r.param = param; ctx->r.priv = (void *)priv;
	ctx->sender = muidl_get_sender();
	ctx->tag = muidl_get_tag();
}


void _set_confirm(rollback_fn_t fn, L4_Word_t param, const void *priv)
{
	struct rbctx *ctx = get_ctx();
	assert(ctx->c.fn == NULL);	/* must be empty */
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
		L4_Word_t param = ctx->c.param;
		void *priv = ctx->c.priv;
		ctx->c.fn = NULL;
		(*fn)(param, priv);
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
	} else if(likely(ctx != NULL)
		&& ctx->tag.raw == muidl_get_tag().raw
		&& ctx->sender.raw == muidl_get_sender().raw)
	{
		/* pop it, where available */
		if(ctx->r.fn != NULL) {
			rollback_fn_t fn = ctx->r.fn;
			ctx->r.fn = NULL;
			(*fn)(ctx->r.param, ctx->r.priv);
			yep = true;
		}
		/* and clear the confirm handler as well */
		ctx->c.fn = NULL;
	}
	if(ctx != NULL) {
		/* clear the rollback even when it wasn't matched, since that means
		 * the reply actually associated with the rollback did succeed, so the
		 * rollback should be discarded.
		 */
		ctx->r.fn = NULL;
	}

	return yep;
}
