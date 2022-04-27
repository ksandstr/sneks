#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H 1

#include <stdint.h>
#include <stddef.h>

typedef int ssize_t; /* IP32 targets */

/* TODO: get these from posixlike.idl, somehow; and others */
typedef int32_t mode_t;
typedef uint32_t id_t;
typedef id_t uid_t, gid_t;
typedef int32_t pid_t;
#define __pid_t_defined
typedef int64_t off64_t;
typedef off64_t off_t;
typedef int32_t dev_t;
typedef uint32_t ino_t;
typedef int32_t nlink_t, blksize_t, blkcnt_t;

#endif
