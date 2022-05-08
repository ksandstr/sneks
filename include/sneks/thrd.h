#ifndef _SNEKS_THRD_H
#define _SNEKS_THRD_H

#include <l4/types.h>

/* per-runtime interfacey bit for lib/thrd.c */
extern int __thrd_new(L4_ThreadId_t *);
extern int __thrd_destroy(L4_ThreadId_t);
extern const int __thrd_stksize_log2;

/* lib/thrd.c initializer */
extern void __thrd_init(void);

/* lib/tss.c export */
extern void __tss_on_exit(void);

#endif
