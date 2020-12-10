
#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H 1

/* TODO: do this in a way that doesn't force inclusion of <stdint.h>, which
 * may be considered pollution.
 */

#include <stdint.h>

#define FD_SETSIZE 1024

/* size_t depends on the target; use compiler headers. */
#define __need_size_t
#include <stddef.h>
#undef __need_size_t

typedef long ssize_t;

typedef int32_t mode_t;

typedef int32_t id_t;
typedef id_t uid_t, gid_t, pid_t;

typedef int32_t off_t;
typedef int64_t off64_t;


typedef struct {
	uintptr_t __w[FD_SETSIZE / (sizeof(uintptr_t) * 8)];
} fd_set;

/* ... and others */

#endif
