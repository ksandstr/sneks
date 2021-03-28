
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>

#include <ccan/minmax/minmax.h>
#include <ccan/darray/darray.h>

#include <sneks/api/directory-defs.h>

#include "private.h"


#define DIRP_VALID(dirp, ctx) \
	((dirp) != NULL && __fdbits((dirp)->dirfd) != NULL)


struct __stdio_dir
{
	int dirfd;
	off_t tellpos;
	bool end;

	/* Sneks::Directory/getdents result capture and parsing */
	int next;
	uint16_t remain;
	unsigned raw_bytes;
	uint8_t dents_raw[SNEKS_DIRECTORY_DENTSBUF_MAX + sizeof(struct dirent)];
};


DIR *opendir(const char *name)
{
	DIR *dirp = malloc(sizeof *dirp);
	if(dirp == NULL) {
		errno = ENOMEM;
		return NULL;
	}
	dirp->dirfd = open(name, O_DIRECTORY | O_RDONLY);
	if(dirp->dirfd < 0) {
		free(dirp);
		return NULL;
	}

	dirp->tellpos = 0;
	dirp->next = -1;
	dirp->end = false;
	assert(DIRP_VALID(dirp, NULL));
	return dirp;
}


int closedir(DIR *dirp)
{
	if(!DIRP_VALID(dirp, NULL)) {
		errno = EBADF;
		return -1;
	}

	close(dirp->dirfd);
	free(dirp);
	return 0;
}


DIR *fdopendir(int fd)
{
	struct fd_bits *bits = __fdbits(fd);
	if(bits == NULL) return NULL;
	int pos = 0;
	int n = __dir_seekdir(bits->server, bits->handle, &pos);
	if(n != 0) {
		NTOERR(n);
		return NULL;
	}

	DIR *dirp = malloc(sizeof *dirp);
	if(dirp == NULL) {
		errno = ENOMEM;
		return NULL;
	}
	dirp->dirfd = fd;
	dirp->tellpos = pos;
	dirp->next = -1;
	dirp->end = false;
	assert(DIRP_VALID(dirp, NULL));
	return dirp;
}


int dirfd(DIR *dirp)
{
	if(!DIRP_VALID(dirp, NULL)) {
		errno = EBADF;
		return -1;
	}

	return dirp->dirfd;
}


struct dirent *readdir(DIR *dirp)
{
	if(!DIRP_VALID(dirp, &ctx)) {
		errno = EBADF;
		return NULL;
	}
	if(dirp->end) return NULL;

	if(dirp->next < 0) {
		const int read_start = max_t(int, 0,
			(int)sizeof(struct sneks_directory_dentry) -
				offsetof(struct dirent, d_name));
		struct fd_bits *bits = __fdbits(dirp->dirfd);
		int offset = dirp->tellpos, endpos = -1;
		dirp->raw_bytes = SNEKS_DIRECTORY_DENTSBUF_MAX;
		int n = __dir_getdents(bits->server, &dirp->remain, bits->handle,
			&offset, &endpos, &dirp->dents_raw[read_start], &dirp->raw_bytes);
		if(n != 0) {
			NTOERR(n);
			return NULL;
		}
		if(dirp->remain == 0) {
			dirp->end = true;
			return NULL;
		}
		dirp->next = read_start;
	}

	/* rewrite Sneks::Directory::dentry to <struct dirent>, retaining the name
	 * field in place.
	 */
	struct sneks_directory_dentry raw =
		*(struct sneks_directory_dentry *)&dirp->dents_raw[dirp->next];
	struct dirent *dent = (void *)&dirp->dents_raw[dirp->next + sizeof raw
		- offsetof(struct dirent, d_name)];
	assert((void *)dent >= (void *)&dirp->dents_raw[0]);
	assert((void *)dent + sizeof *dent < (void *)&dirp->dents_raw[sizeof dirp->dents_raw]);
	assert(&dent->d_name[0] == (char *)&dirp->dents_raw[dirp->next + sizeof raw]);
	*dent = (struct dirent){
		.d_ino = raw.ino, .d_off = raw.off, .d_type = raw.type,
		.d_reclen = sizeof *dent + raw.namlen + 1,
	};
	assert(dent->d_name[raw.namlen] == '\0');
	assert(strcmp(dent->d_name, (char *)&dirp->dents_raw[dirp->next + sizeof raw]) == 0);

	if(--dirp->remain == 0) dirp->next = -1;
	else {
		dirp->next += raw.reclen;
		if(dirp->next >= dirp->raw_bytes - sizeof raw) {
			/* server returned damaged data; go to next chunk. */
			dirp->next = -1;
		}
	}
	dirp->tellpos = dent->d_off;
	return dent;
}


void seekdir(DIR *dirp, long loc)
{
	if(!DIRP_VALID(dirp, &ctx)) return;

	struct fd_bits *bits = __fdbits(dirp->dirfd);
	__dir_seekdir(bits->server, bits->handle, &(int){ loc });
	/* sadly there is no chance to report this error. */
	dirp->tellpos = loc;
	dirp->next = -1;
	dirp->end = false;
}


long telldir(DIR *dirp)
{
	if(!DIRP_VALID(dirp, NULL)) {
		errno = EBADF;
		return -1;
	}

	return dirp->tellpos;
}


void rewinddir(DIR *dirp) {
	seekdir(dirp, 0);
}


int scandir(const char *dir_name, struct dirent ***namelist,
	int (*filter)(const struct dirent *),
	int (*compar)(const struct dirent **, const struct dirent **))
{
	DIR *dirp = opendir(dir_name);
	if(dirp == NULL) return -1;

	darray(struct dirent *) result = darray_new();
	struct dirent *dent;
	int err;
	while(errno = 0, dent = readdir(dirp), dent != NULL) {
		if(filter != NULL && (*filter)(dent) == 0) continue;
		struct dirent *copy = malloc(dent->d_reclen);
		if(copy == NULL) {
			err = ENOMEM;
			goto fail;
		}
		memcpy(copy, dent, dent->d_reclen);
		darray_push(result, copy);
	}
	err = errno;
	closedir(dirp);
	if(err == 0) {
		if(darray_empty(result)) {
			*namelist = NULL;
			darray_free(result);
			return 0;
		} else {
			if(compar != NULL) {
				qsort(result.item, result.size, sizeof(struct dirent *),
					(int (*)(const void *, const void *))compar);
			}
			/* NOTE: may return excess. */
			*namelist = result.item;
			return result.size;
		}
	} else {
		struct dirent **dpp;
fail:
		darray_foreach(dpp, result) {
			free(*dpp);
		}
		darray_free(result);
		errno = err;
		return -1;
	}
}


int alphasort(const struct dirent **a, const struct dirent **b) {
	return strcmp((*a)->d_name, (*b)->d_name);
}
