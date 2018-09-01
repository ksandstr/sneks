
/* derived from fake_stdio.c of mung, relicensed under GPLv2+ for sneks. */

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#include <l4/types.h>

#include "io-defs.h"
#include "private.h"


struct __stdio_file
{
	int fd;
};


static struct __stdio_file stdout_file = { .fd = 1 },
	stderr_file = { .fd = 2 };

FILE *stdout = &stdout_file, *stderr = &stderr_file;


int vfprintf(FILE *stream, const char *fmt, va_list args)
{
	va_list copy;
	va_copy(copy, args);
	int length = vsnprintf(NULL, 0, fmt, copy);
	va_end(copy);
	if(length < 0) return length;

	char buffer[length + 1];
	int n = vsnprintf(buffer, length + 1, fmt, args);
	if(n < 0) return n;
	return fputs(buffer, stream) >= 0 ? n : -1;
}


int fprintf(FILE *stream, const char *fmt, ...)
{
	va_list al;
	va_start(al, fmt);
	int n = vfprintf(stream, fmt, al);
	va_end(al);
	return n;
}


int printf(const char *fmt, ...)
{
	va_list al;
	va_start(al, fmt);
	int n = vfprintf(stdout, fmt, al);
	va_end(al);
	return n;
}


int snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list al;
	va_start(al, fmt);
	int n = vsnprintf(buf, size, fmt, al);
	va_end(al);
	return n;
}


int puts(const char *s)
{
	int len = strlen(s);
	char tmp[len + 2];
	strscpy(tmp, s, len + 1);
	tmp[len] = '\n';
	tmp[len + 1] = '\0';
	return fputs(tmp, stdout);
}


int fputs(const char *s, FILE *stream)
{
	assert(IS_FD_VALID(stream->fd));	/* implied by stream != NULL */
	uint16_t ret;
	int n = __io_write(FD_SERVICE(stream->fd), &ret,
		FD_COOKIE(stream->fd), (void *)s, strlen(s));
	if(n == 0) return ret;
	else {
		/* FIXME: recover POSIX errno */
		return -1;
	}
}


int putchar(char c) {
	return fputc(c, stdout);
}


int fputc(char c, FILE *stream)
{
	char s[2] = { c, '\0' };
	return fputs(s, stream);
}


size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	assert(stream == stdout || stream == stderr);
	const char *cs = ptr;
	for(size_t i=0; i < size * nmemb; i++) fputc(cs[i], stream);
	return nmemb;
}


int fflush(FILE *stream)
{
	/* no buffers, so nothing to flush. */
	return 0;
}
