
/* exports from lib/hash.c */

#ifndef __SNEKS_HASH_H__
#define __SNEKS_HASH_H__

#include <stdint.h>


extern uint32_t int_hash(uint32_t key);
#define word_hash(x) int_hash((uint32_t)(x))

extern uint32_t int64_hash(uint64_t key);


#endif
