
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <ccan/compiler/compiler.h>
#include <sneks/process.h>

#include <l4/types.h>

#include "private.h"


struct __sneks_file *__files = NULL;

static int max_valid_fd = -1;	/* valid as in memory, not IS_FD_VALID() */


/* this isn't as much cold as init-only. */
COLD void __file_init(const struct sneks_fdlist *fdlist)
{
	if(fdlist == NULL) return;
	assert(max_valid_fd < 0);

	max_valid_fd = fdlist->fd;
	__files = calloc(max_valid_fd + 1, sizeof *__files);
	if(__files == NULL) abort();	/* callstack breadcrumbs > segfault */
	int prev = max_valid_fd;
	while(fdlist->next != 0) {
		if(fdlist->fd > prev) abort();	/* invalid fdlist */
		prev = fdlist->fd;
		struct __sneks_file *f = &__files[fdlist->fd];
		f->service = fdlist->serv;
		f->cookie = fdlist->cookie;
		fdlist = sneks_fdlist_next(fdlist);
	}

	/* TODO: chuck @fdlist, or arrange caller to do so */
}
