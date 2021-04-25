
#ifndef _SYS_FCNTL_H
#define _SYS_FCNTL_H 1


#define O_ACCMODE 3

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2

/* TODO: get all of these from io-defs.h, file-defs.h, etc. and group
 * according to what goes in open/openat flags and what in F_SETFL.
 */
#define O_CREAT		0100
#define O_TRUNC		01000
#define O_APPEND	02000
#define O_NONBLOCK	04000
#define O_DIRECTORY	0200000
#define O_TMPFILE	020000000

/* fcntl commands. */
#define F_DUPFD 0
#define F_GETFD 1	/* get/set fd flags (FD_*) */
#define F_SETFD 2
#define F_GETFL 3	/* get/set status flags (O_*) */
#define F_SETFL 4

#define AT_FDCWD -1	/* *at() family @dirfd special value */


extern int open(const char *pathname, int flags, ... /* mode_t mode */);
extern int openat(int dirfd, const char *pathname,
	int flags, ... /* mode_t mode */);

extern int fcntl(int fd, int cmd, ... /* arg */);


#endif
