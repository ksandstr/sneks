#ifndef _SNEKS_SIGNAL_H
#define _SNEKS_SIGNAL_H

#include <stdint.h>
#include <assert.h>
#include <signal.h>

/* Sneks::Proc/sigset represents signal masks as an u64. however, sigset_t is
 * typically defined larger than that. these functions go from sigset_t to u64
 * and back again.
 */
static inline uint64_t __set2mask(const sigset_t *set)
{
	if(sizeof set->__bits[0] == sizeof(uint64_t)) {
		return set->__bits[0];
	} else {
		static_assert(8 * sizeof set->__bits[0] == 32);
		return (uint64_t)set->__bits[0] | ((uint64_t)set->__bits[1] << 32);
	}
}

static inline sigset_t __mask2set(uint64_t mask)
{
	if(sizeof ((sigset_t *)0)->__bits[0] == sizeof(uint64_t)) {
		return (sigset_t){ .__bits = { mask } };
	} else {
		static_assert(8 * sizeof ((sigset_t *)0)->__bits[0] == 32);
		return (sigset_t){ .__bits = { mask & 0xffffffff, mask >> 32 } };
	}
}

#endif
