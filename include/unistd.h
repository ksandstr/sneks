
#ifndef _UNISTD_H
#define _UNISTD_H 1

#include <stdint.h>
#include "fake_clib/unistd.h"


extern int close(int fd);
extern long read(int fd, void *buf, size_t count);

#endif
