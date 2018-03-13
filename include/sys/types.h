
#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H 1

/* TODO: do this in a way that doesn't force inclusion of <stdint.h>, which
 * may be considered pollution. (or it might not matter; these includes are
 * only for the systask runtime anyway.)
 */

#include <stdint.h>


typedef uint32_t mode_t;
typedef int32_t pid_t;

/* ... and others */

#endif
