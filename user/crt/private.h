
/* bits and bobs of userspace runtime that aren't exported to the programs
 * it's linked to. initializers and so forth.
 */

#ifndef _SNEKS_USER_CRT_PRIVATE_H
#define _SNEKS_USER_CRT_PRIVATE_H

#include <l4/types.h>
#include <l4/kip.h>

#include <sneks/sysinfo.h>


/* invalid when .service is nil. */
struct __sneks_file {
	L4_ThreadId_t service;
	L4_Word_t cookie;
};


#define IS_FD_VALID(fd) !L4_IsNilThread(__files[(fd)].service)
#define FD_SERVICE(fd) __files[(fd)].service
#define FD_COOKIE(fd) __files[(fd)].cookie


extern L4_KernelInterfacePage_t *__the_kip;
extern struct __sysinfo *__the_sysinfo;

extern L4_ThreadId_t __main_tid;


/* what file descriptors index into. */
extern struct __sneks_file *__files;


struct sneks_fdlist;
extern void __file_init(struct sneks_fdlist *fdlist);


/* from sigaction.c */
extern void __sig_bottom(void);
extern void __attribute__((regparm(3))) __sig_invoke(int sig);


/* from uid.c */
extern void __init_crt_cached_creds(void);


#endif
