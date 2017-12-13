
#ifndef __SNEKS_MM_H__
#define __SNEKS_MM_H__

#include <string.h>
#include <ccan/minmax/minmax.h>


#define PAGE_BITS 12
#define PAGE_MASK ((1u << PAGE_BITS) - 1)
#define PAGE_SIZE (PAGE_MASK + 1)


/* usage:
 *
 * int size_log2;
 * L4_Word_t address;
 * for_page_range(start, end, address, size_log2) {
 *       L4_Fpage_t page = L4_FpageLog2(address, size_log2);
 *       // carry on
 * }
 *
 * "start" and "end" define an exclusive range, i.e. [start .. end). The null
 * range (i.e. start == end) skips the loop body entirely.
 */
#define for_page_range(_start, _end, _addr, _sizelog2) \
	for(L4_Word_t _E = (_end) & ~PAGE_MASK, _A = (_addr) = (_start) & ~PAGE_MASK, \
			_S = (_sizelog2) = min_t(unsigned, ffsl(_A) - 1, MSB(_E - _A)); \
		_A < _E; \
		(_addr) = (_A += (1 << _S)), \
			(_sizelog2) = _S = min_t(unsigned, ffsl(_A) - 1, MSB(_E - _A)))


#endif
