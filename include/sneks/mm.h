
#ifndef _SNEKS_MM_H
#define _SNEKS_MM_H

#include <stdbool.h>
#include <string.h>
#include <ccan/minmax/minmax.h>
#include <l4/types.h>
#include <sneks/bitops.h>


#ifdef __SNEKS__

#define PAGE_BITS 12
#define PAGE_MASK ((1u << PAGE_BITS) - 1)
#define PAGE_SIZE (PAGE_MASK + 1)

#define PAGE_CEIL(x) (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))



/* memory attributes for Sysmem::alter_flags */
#define SMATTR_PIN 1	/* disallows replacement once mapped */


/* usage:
 *
 * int size_log2;
 * L4_Word_t address;
 * for_page_range(start, end, address, size_log2) {
 *       L4_Fpage_t page = L4_FpageLog2(address, size_log2);
 *       // carry on
 * }
 *
 * iterate through aligned power-of-two pages between [start, end). skip the
 * loop body when start >= end.
 */
#define for_page_range(_start, _end, _addr, _sizelog2) \
	for(L4_Word_t _E = (_end) & ~PAGE_MASK, _A = (_addr) = (_start) & ~PAGE_MASK, \
			_S = (_sizelog2) = min_t(unsigned, ffsl(_A) - 1, MSB(_E - _A)); \
		_A < _E; \
		(_addr) = (_A += (1 << _S)), \
			(_sizelog2) = _S = min_t(unsigned, ffsl(_A) - 1, MSB(_E - _A)))


/* upper bound of the number of iterations for_page_range() runs for the given
 * range.
 */
#define page_range_bound(start, end) ((MSB((end) - (start)) - PAGE_BITS) * 2)


#define ADDR_IN_FPAGE(haystack, needle) \
	fpage_overlap((haystack), L4_FpageLog2((needle), 0))


static inline bool fpage_overlap(L4_Fpage_t a, L4_Fpage_t b)
{
	L4_Word_t mask = ~((1ul << max_t(int, L4_SizeLog2(a), L4_SizeLog2(b))) - 1);
	return ((a.raw ^ b.raw) & mask) == 0;
}

#endif

#endif
