
#ifndef __VM_DEFS_H__
#define __VM_DEFS_H__

#include <l4/types.h>

#include <sneks/sysinfo.h>


extern const struct __sysinfo *the_sip;


/* this leaves an IPC chain to the roottask open. caller should reply MR0=0 to
 * *peer_tid_p when vm is about to enter the IPC loop.
 */
extern L4_Fpage_t *init_protocol(int *n_phys_p, L4_ThreadId_t *peer_tid_p);

#endif
