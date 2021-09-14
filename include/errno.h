
#ifndef _ERRNO_H
#define _ERRNO_H

/* pull error codes from wherever, as required... */

#define EPERM	1	/* operation not permitted */
#define ENOENT	2	/* no such file or directory */
#define ESRCH	3	/* no such process */
#define EINTR	4	/* interrupted system call */
#define EIO		5	/* i/o error */
#define E2BIG	7	/* argument list too long */
#define ENOEXEC	8	/* executable format error */
#define EBADF	9	/* bad file number */
#define ECHILD	10	/* no child processes */
#define EAGAIN	11	/* try again */
#define ENOMEM	12	/* out of memory */
#define EACCES	13	/* access check failed */
#define EFAULT	14	/* bad address */
#define EBUSY	16	/* device or resource busy */
#define EEXIST	17	/* file exists */
#define ENODEV	19	/* no such device */
#define ENOTDIR	20	/* not a directory */
#define EISDIR	21	/* is a directory */
#define EINVAL	22	/* invalid value */
#define ENFILE	23	/* file table overflow (system) */
#define EMFILE	24	/* too many open files (process) */
#define ENOSPC	28	/* no space left on device */
#define ESPIPE	29	/* 's pipe, socket, or fifo (illegal seek) */
#define EROFS	30	/* read-only file system */
#define EPIPE	32	/* broken pipe */
#define ERANGE	34	/* out of range */
#define EDEADLK 35	/* resource deadlock would occur */
#define ENAMETOOLONG 36 /* would exceed PATH_MAX */
#define ENOSYS	38	/* function not implemented */
#define ELOOP	40	/* too many symbolic links encountered */
#define EWOULDBLOCK 41	/* NOTE: distinct from EAGAIN! */
#define EOVERFLOW 75	/* value too large for defined data type */
#define ETIMEDOUT 110	/* connection timed out */


/* TODO: remove this once CCAN intmap no longer insists that errno belongs to
 * it entirely.
 */
#ifndef errno
extern int *__errno_location(void);
#define errno (*__errno_location())
#elif !defined(CCAN_INTMAP_WAS_BORN_TOO_EARLY_NOT_TO_BE_A_PIECE_OF_FRIENDLY_SNOT)
#error "you done fucked up!"
#endif

#ifdef _GNU_SOURCE
extern char *program_invocation_name, *program_invocation_short_name;
#endif

#endif
