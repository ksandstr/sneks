
#ifndef _SYS_FCNTL_H
#define _SYS_FCNTL_H 1


#define O_ACCMODE 3

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2

#define O_TRUNC 01000
#define O_APPEND 02000
#define O_CREAT 00000100
#define O_TMPFILE 020000000


extern int open(const char *pathname, int flags, ...);

#endif
