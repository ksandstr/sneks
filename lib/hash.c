
/* useful hash functions. exported in <sneks/hash.h>. */

#include <stdint.h>

#define SNEKS_LIB_HASH_IMPL 1
#include <sneks/hash.h>


/* probably this is slower than need be. but it'll do, and it's two distinct
 * dep chains anyway so maybe not so much slower.
 */
uint32_t int64_hash(uint64_t key) {
	return hash32shiftmult(key) ^ hash32shiftmult(key >> 32);
}


/* Robert Jenkins' 96-bit mix function. a and b are initialized random bits
 * and the 32-bit input is c.
 */
uint32_t bob96bitmix(uint32_t a, uint32_t b, uint32_t c)
{
	a=a-b;  a=a-c;  a=a^(c >> 13);
	b=b-c;  b=b-a;  b=b^(a << 8);
	c=c-a;  c=c-b;  c=c^(b >> 13);
	a=a-b;  a=a-c;  a=a^(c >> 12);
	b=b-c;  b=b-a;  b=b^(a << 16);
	c=c-a;  c=c-b;  c=c^(b >> 5);
	a=a-b;  a=a-c;  a=a^(c >> 3);
	b=b-c;  b=b-a;  b=b^(a << 10);
	c=c-a;  c=c-b;  c=c^(b >> 15);
	return c;
}


/* via Bob and C-ized; presumed to have been in the public domain, continues
 * to be if so.
 */
uint32_t hash32shiftmult(uint32_t key)
{
	uint32_t c2 = 0x27d4eb2du;	/* a prime or an odd constant */
	key = (key ^ 61) ^ (key >> 16);
	key = key + (key << 3);
	key = key ^ (key >> 4);
	key = key * c2;
	key = key ^ (key >> 15);
	return key;
}


uint32_t int_hash(uint32_t x) {
	return hash32shiftmult(x);
}
