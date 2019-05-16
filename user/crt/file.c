
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <ccan/compiler/compiler.h>
#include <ccan/array_size/array_size.h>
#include <sneks/process.h>

#include <l4/types.h>

#include "private.h"


/* NOTE: statically allocated when max_valid_fd < 8. */
struct __sneks_file *__files = NULL;
int __max_valid_fd = -1;	/* valid as in memory, not IS_FD_VALID() */


/* this isn't as much cold as init-only. */
COLD void __file_init(struct sneks_fdlist *fdlist)
{
	if(fdlist == NULL) return;
	assert(__max_valid_fd < 0);

	static struct __sneks_file first_files[8];
	if(fdlist == NULL || fdlist->fd < ARRAY_SIZE(first_files)) {
		__max_valid_fd = ARRAY_SIZE(first_files) - 1;
		__files = first_files;
		for(int i=0; i < ARRAY_SIZE(first_files); i++) {
			first_files[i] = (struct __sneks_file){ };
		}
	} else {
		__max_valid_fd = fdlist->fd;
		__files = calloc(__max_valid_fd + 1, sizeof *__files);
		if(__files == NULL) abort();	/* callstack breadcrumbs > segfault */
	}
	int prev = fdlist->fd;
	while(fdlist->next != 0) {
		if(fdlist->fd > prev) abort();	/* invalid fdlist */
		prev = fdlist->fd;
		struct __sneks_file *f = &__files[fdlist->fd];
		f->service = fdlist->serv;
		f->cookie = fdlist->cookie;
		fdlist = sneks_fdlist_next(fdlist);
	}

	stdin = fdopen(0, "r");
	stdout = fdopen(1, "w");
	stderr = fdopen(2, "w");
}
