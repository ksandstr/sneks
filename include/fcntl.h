#ifndef _FCNTL_H
#define _FCNTL_H

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_ACCMODE 3

/* TODO: get per IDL */
#define O_CREAT		0100
#define O_TRUNC		01000
#define O_APPEND	02000
#define O_NONBLOCK	04000
#define O_DIRECTORY	0200000
#define O_CLOEXEC	02000000
#define O_TMPFILE	020000000

#define F_DUPFD 0
#define F_GETFD 1	/* get/set fd flags (FD_*) */
#define F_SETFD 2
#define F_GETFL 3	/* get/set status flags (O_*) */
#define F_SETFL 4

#define FD_CLOEXEC 1

#define AT_FDCWD -1	/* *at() family @dirfd special value */

extern int open(const char *pathname, int flags, ... /* mode_t mode */);
extern int openat(int dirfd, const char *pathname, int flags, ... /* mode_t mode */);
extern int fcntl(int fd, int cmd, ... /* arg */);

#endif
