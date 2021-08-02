
#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <sys/types.h>


/* for mmap(2) */
#define PROT_NONE	0
#define PROT_READ	1
#define PROT_WRITE	2
#define PROT_EXEC	4

#define MAP_SHARED	1
#define MAP_PRIVATE	2

#define MAP_FILE	0
#define MAP_FAILED (void *)-1
#define MAP_FIXED 0x10		/* succeed at @addr or fail. */
#define MAP_ANONYMOUS 0x20


extern void *mmap(
	void *addr, size_t length, int prot, int flags,
	int fd, off_t offset);

extern int munmap(void *addr, size_t length);


#endif
