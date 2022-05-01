#ifndef _SNEKS_MSG_H
#define _SNEKS_MSG_H

#include <stdbool.h>
#include <l4/types.h>

#define MSGB_PROCESS_LIFECYCLE 0	/* see <sneks/process.h> */

/* handlers should return true when the message was either disregarded or
 * processed immediately, and false if the non-filter effect of processing the
 * message was only staged to occur later.
 */
typedef bool (*sysmsg_handler_fn)(int bit, L4_Word_t *body, int length, void *priv);

/* listen for broadcasts on 0 <= @bit < WORD_SIZE, delivering the message to
 * all @fn listening in order of listen call. @priv is passed. returns a
 * handle associated with the triple of bit * fn * priv.
 */
extern int sysmsg_listen(int bit, sysmsg_handler_fn fn, void *priv);

/* filter calls of @handle such that only those will be passed where the first
 * word matches any @labels[0 .. @n_labels - 1] previously given. others may
 * also be passed, i.e. false positives are possible. return value is positive
 * L4 ErrorCode on IPC failure.
 *
 * filters are not removed from a handle at close. users should do this
 * explicitly.
 */
extern int sysmsg_add_filter(int handle, const L4_Word_t *labels, int n_labels);

/* same but removes from filter. result is undefined and likely very wrong if
 * items weren't previously added. (some subset of such fail is caught by
 * assertions, but other damage is possible.)
 */
extern int sysmsg_rm_filter(int handle, const L4_Word_t *labels, int n_labels);

/* send @body[0..@length-1] to listeners whose aggregate mask of listeners
 * contains all bits set in @maskp, and doesn't contain any of bits set in
 * @maskn. returns 0 when the underlying call to Sysmsg::broadcast returned
 * true, 1 when it returned false, and negative errno if the message could not
 * be sent at all.
 */
extern int sysmsg_broadcast(int maskp, int maskn, const L4_Word_t *body, int length);

/* discard @handle. idempotent on failure, returns negative errno or 0 on
 * success. filters are not removed from a handle at close; users should do
 * this explicitly.
 */
extern int sysmsg_close(int handle);

#endif
