/* lwIP architecture glue for VTesse (POSIX, GCC/Clang). */
#ifndef VTESSE_LWIP_ARCH_CC_H
#define VTESSE_LWIP_ARCH_CC_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef BYTE_ORDER
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define BYTE_ORDER BIG_ENDIAN
#else
#define BYTE_ORDER LITTLE_ENDIAN
#endif
#endif

#define LWIP_PLATFORM_DIAG(x)   do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { \
        fprintf(stderr, "lwIP assertion \"%s\" failed at %s:%d\n", x, __FILE__, __LINE__); \
        abort(); \
    } while (0)

uint32_t nc_random32(void);
#define LWIP_RAND() ((u32_t)nc_random32())

#endif
