/* efficient, polite spinning. */
#ifndef _SNEKS_SPIN_H
#define _SNEKS_SPIN_H

#include <l4/types.h>
#include <l4/kip.h>

typedef struct {
	L4_Clock_t last;
	int count;
} spinner_t;

extern L4_KernelInterfacePage_t *__the_kip;

extern void __thrd_spin(spinner_t *);
#define spin(s) __thrd_spin((s))

#endif
