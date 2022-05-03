#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>

#define EOF (-1)

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* third argument to `setvbuf()'. */
#define _IOFBF 0	/* block ("fully") buffered */
#define _IOLBF 1	/* line buffered (flush on '\n', or full) */
#define _IONBF 2	/* not buffered */

#define BUFSIZ 8192

struct __stdio_file;
typedef struct __stdio_file FILE;
extern void __stdio_fclose_all(void);

extern FILE *stdin, *stdout, *stderr;

extern int printf(const char *restrict fmt, ...)
	__attribute__((format(printf, 1, 2)));
extern int fprintf(FILE *stream, const char *restrict fmt, ...)
	__attribute__((format(printf, 2, 3)));
extern int vfprintf(FILE *stream, const char *restrict fmt, va_list args);

extern int sprintf(char *restrict buf, const char *restrict fmt, ...)
	__attribute__((format(printf, 2, 3)));
extern int snprintf(char *restrict buf, size_t size, const char *restrict fmt, ...)
	__attribute__((format(printf, 3, 4)));
extern int vsnprintf(char *restrict buf, size_t size, const char *restrict fmt, va_list arg_list);

/* stream operations. */
#ifdef _GNU_SOURCE
typedef ssize_t (cookie_read_function_t)(void *cookie, char *buf, size_t size);
typedef ssize_t (cookie_write_function_t)(void *cookie, const char *buf, size_t size);
typedef int (cookie_seek_function_t)(void *cookie, off64_t *offset, int whence);
typedef int (cookie_close_function_t)(void *cookie);

typedef struct {
	cookie_read_function_t *read;
	cookie_write_function_t *write;
	cookie_seek_function_t *seek;
	cookie_close_function_t *close;
} cookie_io_functions_t;

extern FILE *fopencookie(void *cookie, const char *mode, cookie_io_functions_t io_funcs);
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
extern size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);

extern int fseek(FILE *stream, long offset, int whence);
extern long ftell(FILE *stream);
extern void rewind(FILE *stream);
/* TODO: fgetpos(), fsetpos() */

extern int puts(const char *s);
extern int fputs(const char *s, FILE *stream);
extern int putchar(char c);
extern int fputc(char c, FILE *stream);

extern int fgetc(FILE *stream);
extern char *fgets(char *s, int size, FILE *stream);
/* TODO: getc(), getchar(), ungetc() */

extern void setbuf(FILE *stream, char *buf);
extern int setvbuf(FILE *stream, char *buf, int mode, size_t n);
extern void setbuffer(FILE *stream, char *buf, size_t size);
extern void setlinebuf(FILE *stream);

#endif
