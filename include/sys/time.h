
#ifndef _SYS_TIME_H
#define _SYS_TIME_H 1

#include <stdint.h>


/* FIXME: replace with time_t, suseconds_t */
struct timeval {
	 uint64_t tv_sec;
	 int tv_usec;
};


#endif
