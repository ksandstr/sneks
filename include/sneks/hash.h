#ifndef _SNEKS_HASH_H
#define _SNEKS_HASH_H

#include <stdint.h>
#include <ccan/hash/hash.h>
#include <ccan/compiler/compiler.h>

extern PURE_FUNCTION uint32_t hash32shiftmult(uint32_t key);
#ifndef SNEKS_LIB_HASH_IMPL
#define int_hash(x) hash32shiftmult((x))
#define word_hash(x) int_hash((uint32_t)(x))
#endif

extern PURE_FUNCTION uint32_t int64_hash(uint64_t key);
extern PURE_FUNCTION uint32_t bob96bitmix(uint32_t a, uint32_t b, uint32_t c);

#ifdef CCAN_HTABLE_H
/* htable_get(), but deletes the value before returning it. */
static inline void *htable_pop(struct htable *ht, size_t hash, bool (*cmpfn)(const void *, void *), void *key)
{
	struct htable_iter it;
	for(void *cand = htable_firstval(ht, &it, hash); cand != NULL; cand = htable_nextval(ht, &it, hash)) {
		if((*cmpfn)(cand, key)) {
			htable_delval(ht, &it);
			return cand;
		}
	}
	return NULL;
}
#endif

#endif
