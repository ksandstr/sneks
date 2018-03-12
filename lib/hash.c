
/* useful hash functions. exported in <sneks/hash.h>. */

#include <stdint.h>

#include <sneks/hash.h>


/* hash32shiftmult(); presumed to have been in the public domain. */
uint32_t int_hash(uint32_t key)
{
	uint32_t c2=0x27d4eb2du; // a prime or an odd constant
	key = (key ^ 61) ^ (key >> 16);
	key = key + (key << 3);
	key = key ^ (key >> 4);
	key = key * c2;
	key = key ^ (key >> 15);
	return key;
}


/* probably this is slower than need be. but it'll do, and it's two distinct
 * dep chains anyway so maybe not so much slower.
 */
uint32_t int64_hash(uint64_t key) {
	return int_hash(key) ^ int_hash(key >> 32);
}
