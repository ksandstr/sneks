
/* many syscall interfaces require the caller to take action according to the
 * interface's return data. if the call's receive phase is interrupted (such
 * as due to signal), the return data is lost and the server side should
 * render a repeat call idempotent.
 *
 * this module assists servers implemented using muidl wrt this goal by
 * providing both lazy confirmation and pessimistic rollback, as best suited
 * to each IDL operation. there's also a third mode where a client reply is
 * not handled by the IDL compiler, but instead sleep and wakeup are handled
 * explicitly; those cases are written out by hand and benefit from no library
 * assistance.
 */

#ifndef __SNEKS_ROLLBACK_H__
#define __SNEKS_ROLLBACK_H__

#include <stdbool.h>
#include <l4/types.h>


typedef void (*rollback_fn_t)(L4_Word_t param, void *priv);

/* change the per-thread rollback handler. overwrites previous value, clears
 * when @fn=NULL.
 *
 * the rollback handler will be called at most once by check_rollback() when
 * the ongoing IDL call's reply IPC fails, and cleared if a reply IPC must
 * have succeeded (such as when ReplyWait returns a receive-side error). this
 * makes rollbacks particularly suitable for operations that neither create
 * new objects nor destroy existing ones, since those are typically more
 * involved to undo.
 *
 * because there's some laziness in how IPC failure is matched to rollback,
 * the rollback function should do additional filtering to confirm that it's
 * not being executed after a subsequent call, with matching sender and tag,
 * has a failed reply without having replaced a prior rollback handler. the
 * typical way is to store copies of the values that'd be rolled back, and if
 * the "expected current value" doesn't match, not roll back at all.
 */
extern void set_rollback(rollback_fn_t fn, L4_Word_t param, void *priv);

/* check a _muidl_foo_dispatch() return value and other muidl per-thread data
 * and call a rollback handler if necessary. return true if a rollback was
 * handled.
 */
extern bool check_rollback(L4_Word_t dispatch_status);


/* change the per-thread confirm handler. valid when clear or not already set
 * after the most recent call to sync_confirm(). clears when @fn=NULL.
 *
 * the confirm handler will be called when it's known that the ongoing IDL
 * call's reply IPC has succeeded, and cleared when any subsequent reply half
 * fails. this makes confirm handlers suitable to operations that destroy
 * objects on the server, as those are hard to undo in a rollback handler.
 *
 * all callbacks of the IDL implementation must arrange to call sync_confirm()
 * before accessing state affected by other confirm callbacks, before calling
 * set_confirm() itself, and before returning to the dispatcher; but no more
 * than once per invocation.
 */
extern void set_confirm(rollback_fn_t fn, L4_Word_t param, void *priv);
extern void sync_confirm(void);


#endif
