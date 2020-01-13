
/* macros for sanity-checking various (combinations of) values, such as in
 * asserts and the like. though nothing stops their use in non-debug segments,
 * performance may not be as good as a built-for-purpose tool would have.
 */

#ifndef __SNEKS_SANITY_H__
#define __SNEKS_SANITY_H__

#include <stdint.h>


/* range specified by [addr, addr+size) doesn't wrap around. also excludes the
 * first @addr - 1 actually negative (hence erroneous) values since @size is
 * cast to uintptr_t before subtracting.
 */
#define VALID_ADDR_SIZE(addr, size) \
	(~(uintptr_t)0 - (uintptr_t)(size) >= (uintptr_t)(addr))


#endif
