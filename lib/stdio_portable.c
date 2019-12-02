
/* the non-runtime-specific portion of stdio. is it smart to do all of stdio
 * on top of the fopencookie() extension? yeah it is, until proven otherwise!
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>

#include <ccan/minmax/minmax.h>
#include <ccan/array_size/array_size.h>


struct memopenf {
	void *buf;
	size_t size, pos;
	FILE *stream;
	bool mybuf;
};


FILE *stdin = NULL, *stdout = NULL, *stderr = NULL;

static char first_file_mem[(sizeof(FILE) + sizeof(cookie_io_functions_t)) * 8];
static size_t first_file_pos = 0;


static inline cookie_io_functions_t *GET_IOFNS(FILE *f) {
	return (cookie_io_functions_t *)(f + 1);
}


static FILE *alloc_file(const cookie_io_functions_t *fns)
{
	/* to avoid calling malloc during that fragile part of systemspace bringup
	 * when roottask doesn't yet have a malloc heap, we'll allocate the first
	 * few of these in the BSS segment. this could be disabled for regular
	 * systasks and all of userspace.
	 */
	FILE *f;
	size_t sz = sizeof *f + sizeof *fns;
	if(first_file_pos + sz < sizeof first_file_mem) {
		f = (FILE *)&first_file_mem[first_file_pos];
		first_file_pos += sz;
		first_file_pos = (first_file_pos + 15) & ~15;
		assert(first_file_pos >= 0);
		assert(first_file_pos <= ARRAY_SIZE(first_file_mem));
	} else {
		f = malloc(sz);
	}

	if(f != NULL) {
		*f = (FILE){ .error = 0 };
		*GET_IOFNS(f) = *fns;
	}
	return f;
}


static void free_file(FILE *f)
{
	void *last = (void *)f + sizeof *f + sizeof(cookie_io_functions_t) - 1;
	if(last < (void *)first_file_mem
		|| (void *)f >= (void *)first_file_mem + ARRAY_SIZE(first_file_mem))
	{
		free(f);
	} else if(last == &first_file_mem[first_file_pos]) {
		first_file_pos = (void *)f - (void *)first_file_mem;
		first_file_pos &= ~15;
		assert(first_file_pos >= 0);
		assert(first_file_pos <= ARRAY_SIZE(first_file_mem));
	}
}


/* TODO: export this when stuff starts needing it, such as for fopen() and
 * fdopen()
 */
static int __stdio_parse_modestr(const char *mode)
{
	bool /* binary = false, */ plus = false;
	char major = 0;
	do {
		switch(*mode) {
			case '\0': break;	/* initial zero; ignored */
			case 'r': case 'w': case 'a': major = *mode; break;
			case 'b': /* binary = true; */ break;
			case '+': plus = true; break;
			default:
				errno = EINVAL;
				return -1;
		}
	} while(*(++mode) != '\0');

	if(major == 'r') return plus ? O_RDWR : O_RDONLY;
	else {
		return O_CREAT
			| (plus ? O_RDWR : O_WRONLY)
			| (major == 'a' ? O_APPEND : (plus ? O_TRUNC : 0));
	}
}


FILE *fopencookie(void *cookie, const char *mode, cookie_io_functions_t fns)
{
	FILE *f = alloc_file(&fns);
	if(f == NULL) return NULL;

	f->cookie = cookie;
	f->mode = __stdio_parse_modestr(mode);
	if(f->mode < 0) {
		free_file(f);
		return NULL;
	}

	return f;
}


int fclose(FILE *f)
{
	int n = 0;
	if(GET_IOFNS(f)->close != NULL) {
		n = (*GET_IOFNS(f)->close)(f->cookie);
	}
	free_file(f);
	return n;
}


int fflush(FILE *stream)
{
	/* no buffers, no flushing. let others explain. */
	return 0;
}


size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	if(GET_IOFNS(stream)->read == NULL) return 0;	/* EOF if write only */
	/* TODO: do safe multiplication between @size and @nmemb! (and see
	 * fwrite() about that also.)
	 */
	long n = (*GET_IOFNS(stream)->read)(stream->cookie, ptr, size * nmemb);
	/* note that n/size may be less than @nmemb; for this case the caller
	 * should compare ftell() before and after fread() to determine position
	 * within the stream; or examine @ptr between @size*(n/@size) and
	 * @nmemb*@size for its prewritten background value.
	 */
	return n / size;
}


size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	if(GET_IOFNS(stream)->write == NULL) return 0;
	/* TODO: do safe multiplication between @size and @nmemb! (and see above,
	 * fread() needs the fix as well.)
	 */
	long n = (*GET_IOFNS(stream)->write)(stream->cookie, ptr, size * nmemb);
	/* NOTE: analoguously to what happens for a fread() that hits EOF in the
	 * middle of a @size chunk, we let the caller figure it out. this
	 * interface is therefore unsuited for output into nonseekable streams
	 * with @size > 1.
	 */
	return n / size;
}


int ferror(FILE *stream) {
	return stream->error;
}


void clearerr(FILE *stream) {
	stream->error = 0;
}


int fseek(FILE *stream, long offset, int whence)
{
	if(GET_IOFNS(stream)->seek == NULL) {
		/* not a seekable stream, i.e. one to which positioning applies. */
		errno = EBADF;
		return -1;
	}

	long long off = offset;
	int n = (*GET_IOFNS(stream)->seek)(stream->cookie, &off, whence);
	return n == 0 ? 0 : -1;
}


long ftell(FILE *stream)
{
	if(GET_IOFNS(stream)->seek == NULL) {
		/* see comment in fseek() */
		errno = EBADF;
		return -1;
	}

	long long offset = 0;
	int n = (*GET_IOFNS(stream)->seek)(stream->cookie, &offset, SEEK_CUR);
	return n == 0 ? offset : -1;
}


void rewind(FILE *stream) {
	(void) fseek(stream, 0, SEEK_SET);
}


int vfprintf(FILE *stream, const char *fmt, va_list args)
{
	char buffer[256], *outptr;
	va_list al;
	va_copy(al, args);
	int n = vsnprintf(buffer, sizeof buffer, fmt, al);
	va_end(al);
	if(n < 0) return n;
	else if(n <= sizeof buffer) outptr = buffer;
	else {
		size_t bufsz = n + 1;
		outptr = malloc(bufsz);
		if(outptr == NULL) {
			errno = ENOMEM;
			return -1;
		}

		n = vsnprintf(outptr, bufsz, fmt, args);
		if(n < 0) {
			free(outptr);
			return -1;
		} else if(n >= bufsz) {
			/* hey, fuck you vsnprintf(). */
			outptr[bufsz - 1] = '\0';
		}
	}

	int writesz = n;
	n = fwrite(outptr, sizeof *outptr, writesz, stream);
	if(n < writesz && ferror(stream)) n = -1;
	if(outptr != buffer) free(outptr);
	return n;
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


int putchar(char c) {
	/* wheeeeee */
	return fputc(c, stdout);
}


int fputc(char c, FILE *stream) {
	long n = fwrite(&c, sizeof c, 1, stream);
	return n > 0 ? (unsigned char)c : EOF;
}


int fgetc(FILE *stream)
{
	unsigned char c;
	long n = fread(&c, sizeof c, 1, stream);
	return n > 0 ? c : EOF;
}


char *fgets(char *s, int size, FILE *stream)
{
	/* boy, this'd sure benefit from some sort of read-side buffering. instead
	 * we'll *cough* lean on microarchitectural smarts.
	 */
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


/* fmemopen() and supporting cast. these could live in a separately linkable
 * module, but so could a lot of stdio.
 */

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
		default: Einval:
			errno = EINVAL;
			return -1;
	}

	if(*offset < 0 || *offset > mof->size) goto Einval;
	mof->pos = *offset;

	return 0;
}


static int mof_close(void *cookie)
{
	struct memopenf *mof = cookie;
	if(mof->mybuf) free(mof->buf);
	free(mof);

	return 0;
}


FILE *fmemopen(void *buf, size_t size, const char *mode)
{
	int flags = __stdio_parse_modestr(mode);
	if(flags < 0) return NULL;

	struct memopenf *mof = malloc(sizeof *mof);
	if(mof == NULL) {
		errno = ENOMEM;
		return NULL;
	}
	bool mybuf = false;
	if(buf == NULL) {
		mybuf = true;
		buf = malloc(size);
		if(buf == NULL) {
			errno = ENOMEM;
			free(mof);
			return NULL;
		}
	}
	*mof = (struct memopenf){
		.buf = buf, .size = size, .mybuf = mybuf,
		.pos = (flags & O_APPEND) ? strnlen(buf, size) : 0,
	};
	if(flags & O_TRUNC) {
		assert(~flags & O_APPEND);
		*(char *)buf = '\0';
	}

	mof->stream = fopencookie(mof, mode, (cookie_io_functions_t){
		.read = (flags & O_ACCMODE) == O_RDONLY || (flags & O_ACCMODE) == O_RDWR
			? &mof_read : NULL,
		.write = (flags & O_ACCMODE) == O_WRONLY || (flags & O_ACCMODE) == O_RDWR
			? &mof_write : NULL,
		.seek = &mof_seek,
		.close = &mof_close,
	});
	if(mof->stream == NULL) {
		free(mof);
		return NULL;
	}

	return mof->stream;
}
