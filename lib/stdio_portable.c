#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <ccan/array_size/array_size.h>
#include <ccan/list/list.h>
#include <ccan/minmax/minmax.h>

struct __stdio_file
{
	struct list_node link;	/* in file_list */
	void *cookie;
	int mode;	/* set of O_* plus one of RDONLY/WRONLY/RDWR in O_ACCMASK */
	int error, bufmode;
	char *buffer;
	size_t bufsz, bufmax;
	cookie_io_functions_t fn;
};

struct memopenf
{
	void *buf;
	size_t size, pos;
	FILE *stream;
	bool mybuf;
};

FILE *stdin = NULL, *stdout = NULL, *stderr = NULL;

static struct list_head file_list = LIST_HEAD_INIT(file_list);
static char first_file_mem[(sizeof(FILE) + sizeof(cookie_io_functions_t)) * 8];
static size_t first_file_pos = 0;

static FILE *alloc_file(const cookie_io_functions_t *fns)
{
	/* to avoid calling malloc during that fragile part of systemspace bringup
	 * when roottask doesn't yet have a malloc heap, we'll allocate the first
	 * few of these in the BSS segment. this could be disabled for regular
	 * systasks and all of userspace.
	 */
	FILE *f;
	if(first_file_pos + sizeof *f < sizeof first_file_mem) {
		f = (FILE *)&first_file_mem[first_file_pos];
		first_file_pos += sizeof *f;
		first_file_pos = (first_file_pos + 15) & ~15;
		assert(first_file_pos >= 0 && first_file_pos <= ARRAY_SIZE(first_file_mem));
	} else {
		f = malloc(sizeof *f);
	}
	if(f != NULL) {
		*f = (FILE){ .error = 0, .bufmax = BUFSIZ - 1, .bufmode = _IONBF, .fn = *fns };
		list_add_tail(&file_list, &f->link);
	}
	return f;
}

static void free_file(FILE *f)
{
	list_del_from(&file_list, &f->link);
	void *last = (void *)f + sizeof *f - 1;
	if(last < (void *)first_file_mem || (void *)f >= (void *)first_file_mem + ARRAY_SIZE(first_file_mem)) {
		free(f);
	} else if(last == &first_file_mem[first_file_pos]) {
		first_file_pos = (void *)f - (void *)first_file_mem;
		first_file_pos &= ~15;
		assert(first_file_pos >= 0 && first_file_pos <= ARRAY_SIZE(first_file_mem));
	}
}

static int __stdio_parse_modestr(const char *mode)
{
	int major = 0, plus = false;
	do {
		switch(*mode) {
			case '\0': break;	/* initial zero; ignored */
			case 'r': case 'w': case 'a': major = *mode; break;
			case 'b': /* ignored (we ain't cp/m) */ break;
			case '+': plus = true; break;
			default: errno = EINVAL; return -1;
		}
	} while(*(++mode) != '\0');
	if(major == 'r') return plus ? O_RDWR : O_RDONLY;
	return O_CREAT | (plus ? O_RDWR : O_WRONLY) | (major == 'a' ? O_APPEND : (plus ? O_TRUNC : 0));
}

FILE *fopencookie(void *cookie, const char *mode, cookie_io_functions_t fns)
{
	FILE *f = alloc_file(&fns); if(f == NULL) return NULL;
	f->cookie = cookie;
	f->mode = __stdio_parse_modestr(mode);
	if(f->mode < 0) { free_file(f); return NULL; }
	return f;
}

void *fcookie_NP(FILE *stream) { return stream->cookie; }

int fclose(FILE *f) {
	fflush(f);
	int n = f->fn.close != NULL ? (*f->fn.close)(f->cookie) : 0;
	free_file(f);
	return n;
}

void __stdio_fclose_all(void) {
	FILE *cur, *next;
	list_for_each_safe(&file_list, cur, next, link) {
		fclose(cur);
	}
	assert(list_empty(&file_list));
}

int fflush(FILE *stream)
{
	while(stream->bufsz > 0) {
		ssize_t n = (*stream->fn.write)(stream->cookie, stream->buffer, stream->bufsz);
		assert(n >= 0);
		if(n < stream->bufsz) {
			memmove(stream->buffer, stream->buffer + n, stream->bufsz - n);
			stream->bufsz -= n;
		} else {
			stream->bufsz = 0;
		}
	}
	return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	if(stream->fn.read == NULL) return 0;
	/* TODO: do safe multiplication between @size and @nmemb! (and see
	 * fwrite() about that also.)
	 */
	long n = (*stream->fn.read)(stream->cookie, ptr, size * nmemb);
	/* note that n/size may be less than @nmemb; for this case the caller
	 * should compare ftell() before and after fread() to determine position
	 * within the stream; or examine @ptr between @size*(n/@size) and
	 * @nmemb*@size for its prewritten background value.
	 */
	return n / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	if(stream->fn.write == NULL) return 0;
	if(stream->bufmode != _IONBF && stream->buffer == NULL) {
		stream->buffer = malloc(stream->bufmax + 1);
		if(stream->buffer == NULL) {
			stream->error = ENOMEM;
			return 0;
		}
		assert(stream->bufsz == 0);
	}
	/* TODO: do safe multiplication between @size and @nmemb! (and see above,
	 * fread() needs the fix as well.)
	 */
	size_t remain = size * nmemb, n = 0;
	switch(stream->bufmode) {
		const char *nl;
		default: assert(false); break;
		case _IOLBF:
			while(nl = memchr(ptr, '\n', remain), nl != NULL) {
				if(stream->bufsz > 0) fflush(stream);
				size_t nbytes = nl - (const char *)ptr + 1;
				ssize_t m = (*stream->fn.write)(stream->cookie, ptr, nbytes);
				assert(m >= 0);
				remain -= m; ptr += m; n += m;
				if(m < nbytes) break;
			}
			/* FALL THRU */
		case _IOFBF:
			if(stream->bufsz + remain <= stream->bufmax) {
				memcpy(stream->buffer + stream->bufsz, ptr, remain);
				stream->bufsz += remain; n += remain;
				break;
			}
			if(stream->bufsz > 0) fflush(stream);
			/* FALL THRU */
		case _IONBF:
			n += (*stream->fn.write)(stream->cookie, ptr, remain);
			break;
	}
	/* NOTE: analoguously to what happens for a fread() that hits EOF in the
	 * middle of a @size chunk, we let the caller figure it out. this
	 * interface is therefore unsuited for output into nonseekable streams
	 * with @size > 1.
	 */
	return n / size;
}

int ferror(FILE *stream) { return stream->error; }

void clearerr(FILE *stream) { stream->error = 0; }

int fseek(FILE *stream, long offset, int whence) {
	if(stream->fn.seek == NULL) { errno = EBADF; return -1; }
	return (*stream->fn.seek)(stream->cookie, &(long long){ offset }, whence) == 0 ? 0 : -1;
}

long ftell(FILE *stream) {
	if(stream->fn.seek == NULL) { errno = EBADF; return -1; }
	long long offset = 0;
	int n = (*stream->fn.seek)(stream->cookie, &offset, SEEK_CUR);
	return n == 0 ? offset : -1;
}

void rewind(FILE *stream) { fseek(stream, 0, SEEK_SET); }

int vfprintf(FILE *stream, const char *restrict fmt, va_list args)
{
	char buffer[256], *outptr;
	va_list al; va_copy(al, args);
	int n = vsnprintf(buffer, sizeof buffer, fmt, al);
	va_end(al);
	if(n < 0) return n;
	else if(n <= sizeof buffer) outptr = buffer;
	else {
		size_t bufsz = n + 1;
		if(outptr = malloc(bufsz), outptr == NULL) { errno = ENOMEM; return -1; }
		n = vsnprintf(outptr, bufsz, fmt, args);
		if(n < 0) { free(outptr); return -1; }
		if(n >= bufsz) outptr[bufsz - 1] = '\0';
	}
	int writesz = n;
	n = fwrite(outptr, sizeof *outptr, writesz, stream);
	if(n < writesz && ferror(stream)) n = -1;
	if(outptr != buffer) free(outptr);
	return n;
}

int fprintf(FILE *stream, const char *restrict fmt, ...) {
	va_list al; va_start(al, fmt);
	int n = vfprintf(stream, fmt, al);
	va_end(al);
	return n;
}

int printf(const char *restrict fmt, ...) {
	va_list al; va_start(al, fmt);
	int n = vfprintf(stdout, fmt, al);
	va_end(al);
	return n;
}

int snprintf(char *restrict buf, size_t size, const char *restrict fmt, ...) {
	va_list al; va_start(al, fmt);
	int n = vsnprintf(buf, size, fmt, al);
	va_end(al);
	return n;
}

int puts(const char *s)
{
	int n = fputs(s, stdout);
	if(n > 0) {
		if(fputc('\n', stdout) != '\n') n = 0;
		else n++;
	}
	return n;
}

int fputs(const char *s, FILE *stream) {
	size_t sz = strlen(s);
	long n = fwrite(s, sizeof *s, sz, stream);
	return n != 0 || !ferror(stream) ? n : EOF;
}

int putchar(char c) { return fputc(c, stdout); }

int fputc(char c, FILE *stream) {
	long n = fwrite(&c, sizeof c, 1, stream);
	return n > 0 ? (unsigned char)c : EOF;
}

int fgetc(FILE *stream) {
	unsigned char c;
	long n = fread(&c, sizeof c, 1, stream);
	return n > 0 ? c : EOF;
}

char *fgets(char *s, int size, FILE *stream)
{
	int p = 0;
	while(p < size - 1) {
		int c = fgetc(stream);
		if(c != EOF) s[p++] = c;
		if(c == EOF || c == '\n') break;
	}
	if(p == 0) return NULL;
	else {
		s[p] = '\0';
		return s;
	}
}

int setvbuf(FILE *stream, char *buf, int mode, size_t size)
{
	if(stream->mode != _IONBF && stream->bufsz > 0) { errno = EBADF; return -1; }
	stream->bufmode = mode;
	stream->buffer = buf;
	stream->bufmax = (size > 0 ? size : BUFSIZ) - 1;
	stream->bufsz = 0;
	return 0;
}

void setbuf(FILE *stream, char *buf) { setvbuf(stream, buf, buf != NULL ? _IOFBF : _IONBF, BUFSIZ); }

void setbuffer(FILE *stream, char *buf, size_t size) { setvbuf(stream, buf, buf != NULL ? _IOFBF : _IONBF, size); }

void setlinebuf(FILE *stream) { setvbuf(stream, NULL, _IOLBF, 0); }

/* fmemopen() and friends, could be elsewhere. */
static ssize_t mof_read(void *cookie, char *buf, size_t size)
{
	struct memopenf *mof = cookie;
	size = min(size, mof->size - mof->pos);
	if(size > 0) {
		memcpy(buf, mof->buf + mof->pos, size);
		mof->pos += size;
	}
	return size;
}

static ssize_t mof_write(void *cookie, const char *buf, size_t size)
{
	struct memopenf *mof = cookie;
	size = min(size, mof->size - mof->pos);
	if(size > 0) {
		memcpy(mof->buf + mof->pos, buf, size);
		mof->pos += size;
	}
	return size;
}

static int mof_seek(void *cookie, off64_t *offset, int whence)
{
	struct memopenf *mof = cookie;
	switch(whence) {
		case SEEK_SET: break;
		case SEEK_CUR: *offset += mof->pos; break;
		case SEEK_END: *offset += mof->size; break;
		default: Einval: errno = EINVAL; return -1;
	}
	if(*offset < 0 || *offset > mof->size) goto Einval;
	mof->pos = *offset;
	return 0;
}

static int mof_close(void *cookie) {
	struct memopenf *mof = cookie;
	if(mof->mybuf) free(mof->buf);
	free(mof);
	return 0;
}

FILE *fmemopen(void *buf, size_t size, const char *mode)
{
	int flags = __stdio_parse_modestr(mode); if(flags < 0) return NULL;
	struct memopenf *mof = malloc(sizeof *mof); if(mof == NULL) goto Enomem;
	*mof = (struct memopenf){ .buf = buf, .size = size, .pos = (flags & O_APPEND) && buf != NULL ? strnlen(buf, size) : 0 };
	if(mof->buf == NULL) {
		mof->mybuf = true;
		if(mof->buf = calloc(size, 1), mof->buf == NULL) { free(mof); goto Enomem; }
	} else if(flags & O_TRUNC) {
		assert(~flags & O_APPEND);
		*(char *)buf = '\0';
	}
	mof->stream = fopencookie(mof, mode, (cookie_io_functions_t){
		.read = (flags & O_ACCMODE) == O_RDONLY || (flags & O_ACCMODE) == O_RDWR ? &mof_read : NULL,
		.write = (flags & O_ACCMODE) == O_WRONLY || (flags & O_ACCMODE) == O_RDWR ? &mof_write : NULL,
		.seek = &mof_seek, .close = &mof_close,
	});
	if(mof->stream == NULL) { free(mof); return NULL; }
	return mof->stream;
Enomem: errno = ENOMEM; return NULL;
}
