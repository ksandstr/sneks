
#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H 1

/* TODO: do this in a way that doesn't force inclusion of <stdint.h>, which
 * may be considered pollution.
 */

#include <stdint.h>


typedef uint32_t mode_t;
typedef int32_t pid_t;

typedef int32_t idtype_t, id_t;

/* ... and others */

#endif
