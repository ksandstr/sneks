
/* bits and bobs of userspace runtime that aren't exported to the programs
 * it's linked to. initializers and so forth.
 */

#ifndef _SNEKS_USER_CRT_PRIVATE_H
#define _SNEKS_USER_CRT_PRIVATE_H

#include <l4/types.h>


/* invalid when .service is nil. */
struct __sneks_file {
	L4_ThreadId_t service;
	L4_Word_t cookie;
};


#define IS_FD_VALID(fd) !L4_IsNilThread(__files[(fd)].service)
#define FD_SERVICE(fd) __files[(fd)].service
#define FD_COOKIE(fd) __files[(fd)].cookie


/* what file descriptors index into. */
extern struct __sneks_file *__files;


struct sneks_fdlist;
extern void __file_init(const struct sneks_fdlist *fdlist);


#endif
