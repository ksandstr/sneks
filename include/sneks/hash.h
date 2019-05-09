
/* exports from lib/hash.c */

#ifndef __SNEKS_HASH_H__
#define __SNEKS_HASH_H__

#include <stdint.h>


extern uint32_t int_hash(uint32_t key);
#define word_hash(x) int_hash((uint32_t)(x))

extern uint32_t int64_hash(uint64_t key);
extern uint32_t bob96bitmix(uint32_t a, uint32_t b, uint32_t c);


#endif
