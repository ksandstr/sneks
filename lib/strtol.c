/* string-to-numeric conversions.
 * TODO: add ctype.h, use ctype.h
 * MISSING: strtod(), strtoll()
 */
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <ccan/minmax/minmax.h>

static bool my_isspace(int c) {
	switch(c) {
		case ' ': case '\n': case '\r': case '\t': return true;
		default: return false;
	}
}

static int my_tolower(int c) {
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static unsigned long long strtoull_max(const char *restrict str, char **restrict end, int base, unsigned long long max)
{
	if(base != 0 && (base < 2 || base > 36)) { errno = EINVAL; return 0; }
	const char *orig = str;
	while(my_isspace(*str)) str++;
	bool minus = *str == '-', noconv = true;
	if(minus || *str == '+') str++;
	if((base == 0 || base == 16) && str[0] == '0' && my_tolower(str[1]) == 'x') {
		base = 16; str += 2;
	} else if(base == 0 && str[0] == '0') {
		base = 8; str++;
	} else if(base == 0) {
		base = 10;
	}
	unsigned long long acc = 0, lim = max / base;
	while(*str != '\0') {
		int c = my_tolower(*str), digit;
		switch(c) {
			case '0' ... '9': digit = c - '0'; break;
			case 'a' ... 'z': digit = 10 + c - 'a'; break;
			default: digit = base;
		}
		if(digit >= base) break;
		str++;
		if(acc >= lim && digit > max % base) {
			errno = ERANGE;
			acc = ULLONG_MAX;
			minus = false;
			break;
		}
		acc = acc * base + digit;
		noconv = false;
	}
	if(end != NULL) *end = (char *)(noconv ? orig : str);
	/* doesn't set EINVAL when no conversion occurred. */
	if(minus) acc = -(long long)acc;
	return acc;
}

/* TODO: untested except via strtoul() tests hitting strtoull_max() */
unsigned long long strtoull(const char *restrict str, char **restrict end, int base) {
	return strtoull_max(str, end, base, ULLONG_MAX);
}

unsigned long strtoul(const char *restrict str, char **restrict endptr, int base) {
	return strtoull_max(str, endptr, base, ULONG_MAX);
}

/* TODO: untested wrt signed overflow */
long strtol(const char *restrict nptr, char **restrict endptr, int base) {
	return strtoul(nptr, endptr, base);
}

int atoi(const char *str) {
	return strtol(str, NULL, 10);
}
