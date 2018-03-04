
#include <stdbool.h>
#include <stdlib.h>


/* FIXME: add ctype.h */
static bool my_isspace(char c) {
	switch(c) {
		case ' ': case '\n': case '\r': case '\t':
			return true;
		default:
			return false;
	}
}


static char my_tolower(char c) {
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}


static int ctoi(char c)
{
	if(c >= '0' && c <= '9') return c - '0';
	else return 10 + my_tolower(c) - 'a';
}


/* FIXME: this doesn't detect overflow. */
static unsigned long strtoul_noskip(const char *nptr, char **endptr, int base)
{
	/* prefixes. */
	if((base == 0 || base == 16)
		&& nptr[0] == '0' && my_tolower(nptr[1]) == 'x')
	{
		nptr += 2;
		base = 16;
	} else if((base == 0 || base == 8) && nptr[0] == '0') {
		nptr++;
		base = 8;
	} else if(base == 0) {
		base = 10;
	}

	unsigned long acc = 0;
	while(*nptr != '\0') {
		int digit = ctoi(*nptr);
		if(digit >= base) break;
		acc = (acc * base) + digit;
		nptr++;
	}

	if(endptr != NULL) *endptr = (char *)nptr;
	return acc;
}


unsigned long strtoul(const char *nptr, char **endptr, int base)
{
	while(*nptr != '\0' && my_isspace(*nptr)) nptr++;
	return strtoul_noskip(nptr, endptr, base);
}


/* FIXME: this doesn't detect overflow or underflow. */
long strtol(const char *nptr, char **endptr, int base)
{
	while(*nptr != '\0' && my_isspace(*nptr)) nptr++;
	bool neg = false;
	if(*nptr == '+') nptr++; else if(*nptr == '-') { neg = true; nptr++; }
	unsigned long val = strtoul_noskip(nptr, endptr, base);
	return neg ? -(long)val : val;
}


int atoi(const char *str) {
	return strtol(str, NULL, 10);
}
