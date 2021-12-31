/* FIXME: this should get all its typenames from somewhere in such a way that
 * it doesn't contaminate the user namespace with vanilla versions. i.e.
 * __off_t, __ssize_t, and so forth.
 *
 * FIXME: all the parameter names etc. should also be either doubly
 * underscored so that existing macros don't screw them up, or elided.
 */

#ifndef _STDIO_H
#define _STDIO_H 1

#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>


#define EOF (-1)

/* whence for fseek() */
/* TODO: get these from API decls once find-idl-defs.pl understands the
 * ubiquitous #include <stdio.h>.
 */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* third argument to `setvbuf()'. */
#define _IOFBF 0	/* block ("fully") buffered */
#define _IOLBF 1	/* line buffered (flush on '\n', or full) */
#define _IONBF 2	/* not buffered */

/* default buffer size, as for setbuf(3). tad on the large side to be sure,
 * but microkernel context transfers are slow so let's bloat this right up.
 */
#define BUFSIZ 8192


struct __stdio_file;
typedef struct __stdio_file FILE;
extern void __stdio_fclose_all(void);


extern int printf(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));

extern int fprintf(FILE *stream, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

extern int vfprintf(FILE *stream, const char *fmt, va_list args);

extern int snprintf(char *buf, size_t size, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
extern int sprintf(char *buf, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

extern int vsnprintf(
	char *buf,
	size_t size,
	const char *fmt,
	va_list arg_list);


extern FILE *stdin, *stdout, *stderr;


/* stream operations. */

#ifdef _GNU_SOURCE
typedef ssize_t (cookie_read_function_t)(void *cookie, char *buf, size_t size);
typedef ssize_t (cookie_write_function_t)(
	void *cookie, const char *buf, size_t size);
typedef int (cookie_seek_function_t)(
	void *cookie, off64_t *offset, int whence);
typedef int (cookie_close_function_t)(void *cookie);

typedef struct {
	cookie_read_function_t *read;
	cookie_write_function_t *write;
	cookie_seek_function_t *seek;
	cookie_close_function_t *close;
} cookie_io_functions_t;

extern FILE *fopencookie(
	void *cookie, const char *mode, cookie_io_functions_t io_funcs);
extern void *fcookie_NP(FILE *stream);
#endif

extern FILE *fopen(const char *path, const char *mode);
extern FILE *fdopen(int fd, const char *mode);
extern FILE *fmemopen(void *buf, size_t size, const char *mode);
extern int fclose(FILE *f);
extern int fflush(FILE *stream);

extern int ferror(FILE *stream);
extern void clearerr(FILE *stream);

extern size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
extern size_t fwrite(
	const void *ptr, size_t size, size_t nmemb, FILE *stream);

extern int fseek(FILE *stream, long offset, int whence);
extern long ftell(FILE *stream);
extern void rewind(FILE *stream);
/* TODO: fgetpos(), fsetpos() */

/* some compilers change a simple printf() to a puts(). likewise for other
 * things.
 */
extern int puts(const char *s);
extern int fputs(const char *s, FILE *stream);
extern int putchar(char c);
extern int fputc(char c, FILE *stream);

extern int fgetc(FILE *stream);
extern char *fgets(char *s, int size, FILE *stream);
/* TODO: getc(), getchar(), ungetc() */

/* buffer twiddle */
extern void setbuf(FILE *stream, char *buf);
extern int setvbuf(FILE *stream, char *buf, int mode, size_t n);
extern void setbuffer(FILE *stream, char *buf, size_t size);
extern void setlinebuf(FILE *stream);

#endif
