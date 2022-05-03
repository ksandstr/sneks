#ifndef _ALLOCA_H
#define _ALLOCA_H

#ifdef __GNUC__
#define alloca(size) __builtin_alloca((size))
#else
#error "don't know how to alloca on this compiler!"
#endif

#endif
