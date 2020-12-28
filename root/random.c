
/* random number generation using the XTEA algorithm from CCAN in some kind of
 * a quarter-arsed CTR mode. it's likely that XTEA is way too powerful and/or
 * expensive for this kind of thing; and definitely the case that hardware AES
 * should be used instead where available.
 */

#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <ccan/minmax/minmax.h>
#include <ccan/crypto/xtea/xtea.h>

#include "defs.h"


static struct xtea_secret xstate;
static uint64_t xplaintext = 0x1023456789abcdefull;
static unsigned perturb_pos;

static union {
	uint64_t u64;
	uint8_t u8[8];
} buffer;
static int buffer_depth = 0;

/* stetson-harrison entropy pool */
static uint64_t epool = 0xfedcba9876543210ull;
static int epool_depth = 0;


/* this can be called any number of times. */
void random_init(uint64_t x) {
	xplaintext ^= x;
	xplaintext = (xplaintext << 63) | (xplaintext >> 1);
}


void add_entropy(uint64_t value)
{
	epool ^= value >> 1;
	int d = value & 1;
	epool = (epool << (2 + d)) | (epool >> (sizeof epool * CHAR_BIT - 2 - d));
	if(value != 0 && ++epool_depth > 64) epool_depth = 64;
}


/* "/dev/urandom" style output. not guaranteed prediction resistant, so only
 * crypto strength for short sequences and when good entropy was available.
 */
void generate_u(void *outbuf, size_t length)
{
	int budget = 4;	/* don't drain the pool entirely */
	size_t done = 0;
	while(done < length) {
		int n;
		if(buffer_depth == 0) {
			if(budget > 0 && epool_depth > 0) {
				uint8_t *sp = &xstate.u.u8[perturb_pos++ & 15];
				*sp ^= epool & 1;
				*sp = (*sp << 1) | (*sp >> 7);
				epool = (epool << 1) | (epool >> 63);
				epool_depth--;
				budget--;
			}
			buffer.u64 = xtea_encipher(&xstate, xplaintext++);
			buffer_depth = 8;
		}
		n = min_t(int, length - done, buffer_depth);
		memcpy(outbuf + done, &buffer.u8[8 - buffer_depth], n);
		buffer_depth -= n;
		done += n;
	}
}
