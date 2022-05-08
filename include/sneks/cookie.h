#ifndef _SNEKS_COOKIE_H
#define _SNEKS_COOKIE_H

#include <stdint.h>
#include <stdbool.h>
#include <l4/types.h>

/* cookies change every 2**15 µs (~32 ms) and remain valid into the next
 * period. consumers should prevent bruteforcing by generational invalidation
 * once at least 6 bits of the cookie space have been covered, i.e. after 64
 * failing attempts within a given period.
 */
#define COOKIE_PERIOD_US ((1 << 15) - 1)

struct cookie_key {
	uint8_t key[16];
};

extern uint32_t gen_cookie(const struct cookie_key *key, L4_Clock_t now, unsigned object, int pid);
extern bool validate_cookie(uint32_t cookie, const struct cookie_key *key, L4_Clock_t now, unsigned object, int pid);

#endif
