
#ifndef _UNISTD_H
#define _UNISTD_H 1

#include <stdint.h>
#include <sys/types.h>

#include "fake_clib/unistd.h"


extern int close(int fd);
extern long read(int fd, void *buf, size_t count);

extern pid_t fork(void);
extern pid_t wait(int *status_p);

#endif
