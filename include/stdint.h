
/* minimal, nonconforming <stdint.h> */

#ifndef _STDINT_H
#define _STDINT_H

#include <stddef.h>


#define INT32_MAX (2147483647)
#define INT32_MIN (-2147483648)


#define UINT64_C(x) x ## ULL


typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;

typedef signed char int8_t;
typedef short int int16_t;
typedef int int32_t;
typedef long long int64_t;

typedef int32_t intptr_t;
typedef uint32_t uintptr_t;


#endif
