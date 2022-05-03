#ifndef _TIME_H
#define _TIME_H

#include <stdint.h>

struct timespec {
	uint64_t tv_sec;
	int tv_nsec;
};

#endif
