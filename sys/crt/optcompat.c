/* quarter-arsed strtod() and such things. these do nothing! this is only here
 * so that CCAN's opt module will link.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>
#include <errno.h>


long long int strtoll(const char *nptr, char **endptr, int base)
{
	printf("%s: not implemented!\n", __func__);
	abort();
}


double strtod(const char *nptr, char **endptr)
{
	printf("%s: not implemented!\n", __func__);
	abort();
}


int ioctl(int fd, unsigned long request, ...)
{
	errno = ENOSYS;
	return -1;
}


/* for some reason, CCAN seems to insist on old-style unbounded string
 * operations. what the heck that's about, I don't know.
 */
int sprintf(char *str, const char *fmt, ...)
{
	va_list al;
	va_start(al, fmt);
	int n = vsnprintf(str, INT_MAX, fmt, al);
	va_end(al);
	return n;
}
