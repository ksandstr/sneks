
/* things from sys/crt that're available to systasks, and not to root, sysmem,
 * or userspace.
 */

#ifndef __SNEKS_SYSTASK_H__
#define __SNEKS_SYSTASK_H__

#include <stdbool.h>
#include <l4/types.h>


/* from threads.c */
extern L4_ThreadId_t __uapi_tid;


#endif
