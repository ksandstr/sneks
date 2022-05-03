#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#include <stdint.h>
#include <time.h>

/* FIXME: replace with time_t, suseconds_t */
struct timeval {
	 uint64_t tv_sec;
	 int tv_usec;
};

#endif
