
/* private bits within sys/crt. */

#ifndef _SNEKS_SYS_CRT_PRIVATE_H
#define _SNEKS_SYS_CRT_PRIVATE_H

#include <stdbool.h>
#include <l4/types.h>


/* from crt1.c */
extern L4_ThreadId_t __get_rootfs(bool *root_mounted_p);


/* from threads.c */
extern void __thrd_init(void);


#endif
