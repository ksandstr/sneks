
#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H 1

/* TODO: do this in a way that doesn't force inclusion of <stdint.h>, which
 * may be considered pollution.
 */

#include <stdint.h>

#define FD_SETSIZE 1024

typedef long ssize_t;

typedef uint32_t mode_t;
typedef int32_t pid_t;

typedef int32_t idtype_t, id_t;

typedef uint32_t __uid_t, __gid_t;

typedef int64_t off64_t;


typedef struct {
	uintptr_t __w[FD_SETSIZE / (sizeof(uintptr_t) * 8)];
} fd_set;

/* ... and others */

#endif
