#ifndef _SNEKS_ROOTSERV_H
#define _SNEKS_ROOTSERV_H

/* long_panic() class parameter. low 8 bits indicate class (i.e. PANIC_*),
 * rest is a mask of PANICF_*.
 */
enum rootserv_panic_class {
	PANIC_UNKNOWN = 0,	/* no known reason */
	PANIC_EXIT = 1,		/* critical systask exited */
	PANIC_BENIGN = 2,	/* reason wasn't unexpected */
};

/* long_panic() flags. */
#define PANICF_SEGV 0x100	/* related to systask segv */

#endif
