/* minimal, nonconforming <stdint.h> */
#ifndef _STDINT_H
#define _STDINT_H

#define INT8_MIN (-128)
#define INT8_MAX (127)
#define UINT8_MAX (255)
#define INT16_MIN (-32768)
#define INT16_MAX (32767)
#define UINT16_MAX (65535)
#define INT32_MIN (-2147483647-1)
#define INT32_MAX (2147483647)
#define UINT32_MAX (4294967295)
#define INT64_MIN (-9223372036854775807ll-1)
#define INT64_MAX (9223372036854775807ll)
#define UINT64_MAX (18446744073709551615ull)

#define INT_FAST8_MIN INT32_MIN
#define INT_FAST8_MAX INT32_MAX
#define UINT_FAST8_MAX UINT32_MAX
#define INT_FAST16_MIN INT32_MIN
#define INT_FAST16_MAX INT32_MAX
#define UINT_FAST16_MAX UINT32_MAX
#define INT_FAST32_MIN INT32_MIN
#define INT_FAST32_MAX INT32_MAX
#define UINT_FAST32_MAX UINT32_MAX
#define INT_FAST64_MIN INT64_MIN
#define INT_FAST64_MAX INT64_MAX
#define UINT_FAST64_MAX UINT64_MAX

#define SIZE_MAX ULONG_MAX

#define UINT64_C(x) x ## ULL

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned int uint_fast8_t;
typedef unsigned int uint_fast16_t;
typedef unsigned int uint_fast32_t;
typedef unsigned long long uint_fast64_t;

typedef signed char int8_t;
typedef short int int16_t;
typedef int int32_t;
typedef long long int64_t;
typedef int int_fast8_t;
typedef int int_fast16_t;
typedef int int_fast32_t;
typedef long long int_fast64_t;

typedef int32_t intptr_t;
typedef uint32_t uintptr_t;

typedef long long intmax_t;
typedef unsigned long long uintmax_t;

#endif
