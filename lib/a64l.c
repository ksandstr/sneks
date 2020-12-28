
/* POSIX long to radix64 alphanumeric conversion (and back again). note that
 * these don't concatenate like an arbitrary-length base64 encoder would do;
 * the 6 bytes output actually encode 36 bits, so there'd be a 4-bit gap in
 * between limbs. a similar use case can be achieved by encoding 24 bits into
 * 4 bytes at a time and adding either separators or padding.
 *
 * the posix manpage is less useful than it could be: among other things, it
 * says that if l64a @value is negative, behaviour is unspecified; so
 * standard-conforming code cannot encode and decode the full range of long
 * integers, which seems the whole bleeding idea of having this encoding in
 * the first place. what we'll do instead is take the first 32 bits unsigned
 * from @value and encode that, and sign-extend the result in a64l().
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <ccan/array_size/array_size.h>


static uint8_t dec_table[] = {
	['.'] = 0, ['/'] = 1,
	['0'] = 2, ['1'] = 3, ['2'] = 4, ['3'] = 5, ['4'] = 6, ['5'] = 7,
	['6'] = 8, ['7'] = 9, ['8'] = 10, ['9'] = 11, ['A'] = 12, ['B'] = 13,
	['C'] = 14, ['D'] = 15, ['E'] = 16, ['F'] = 17, ['G'] = 18, ['H'] = 19,
	['I'] = 20, ['J'] = 21, ['K'] = 22, ['L'] = 23, ['M'] = 24, ['N'] = 25,
	['O'] = 26, ['P'] = 27, ['Q'] = 28, ['R'] = 29, ['S'] = 30, ['T'] = 31,
	['U'] = 32, ['V'] = 33, ['W'] = 34, ['X'] = 35, ['Y'] = 36, ['Z'] = 37,
	['a'] = 38, ['b'] = 39, ['c'] = 40, ['d'] = 41, ['e'] = 42, ['f'] = 43,
	['g'] = 44, ['h'] = 45, ['i'] = 46, ['j'] = 47, ['k'] = 48, ['l'] = 49,
	['m'] = 50, ['n'] = 51, ['o'] = 52, ['p'] = 53, ['q'] = 54, ['r'] = 55,
	['s'] = 56, ['t'] = 57, ['u'] = 58, ['v'] = 59, ['w'] = 60, ['x'] = 61,
	['y'] = 62, ['z'] = 63,
};


static int decode_byte(unsigned c, bool *fail)
{
	if(c >= ARRAY_SIZE(dec_table) || (dec_table[c] == 0 && c != '.')) {
		*fail = true;
		return -1;
	} else {
		return dec_table[c];
	}
}


static int encode_byte(int b)
{
	static const char enc_table[64] =
		"./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	if(b >= 0 && b < ARRAY_SIZE(enc_table)) return enc_table[b];
	else return '&';	/* , you clown */
}


long a64l(const char *str64)
{
	int32_t acc = 0;
	bool fail = false;
	for(int i = 0, scale = 0; i < 6 && str64[i] != '\0'; i++, scale += 6) {
		acc |= decode_byte(str64[i], &fail) << scale;
	}
	return fail ? 0 : acc;
}


char *l64a(long input)
{
	static char str[7];
	uint32_t v = input;
	for(int i=0; i < 6 && v > 0; i++, v >>= 6) {
		str[i] = encode_byte(v & 0x3f);
		str[i+1] = '\0';
	}
	return str;
}
