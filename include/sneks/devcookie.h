
/* defs for lib/devcookie.c */

#ifndef _SNEKS_DEVCOOKIE_H
#define _SNEKS_DEVCOOKIE_H

#include <stdint.h>
#include <stdbool.h>
#include <l4/types.h>


/* timeslices shouldn't exceed 32767 µs; and that shouldn't be enough time to
 * brute force more than (say) 20 bits' worth of the cookie space, IPC
 * roundtrip cost being some 1000 cycles already.
 */
#define COOKIE_PERIOD_US ((1 << 15) - 1)


struct cookie_key {
	unsigned char key[16];
};


extern uint32_t gen_cookie(const struct cookie_key *key,
	L4_Clock_t now, unsigned object, int pid);
extern bool validate_cookie(uint32_t cookie,
	const struct cookie_key *key, L4_Clock_t now, unsigned object, int pid);


#endif
