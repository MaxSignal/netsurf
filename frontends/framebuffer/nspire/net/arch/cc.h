/* lwIP compiler/platform glue for the TI-Nspire (ARM926, newlib). */
#ifndef NNET_ARCH_CC_H
#define NNET_ARCH_CC_H

#include <stdint.h>
#include <stdlib.h>

#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

void nnet_port_log(const char *fmt, ...);
void nnet_lwip_diag(const char *fmt, ...);

#define LWIP_PLATFORM_DIAG(x)       do { nnet_lwip_diag x; } while (0)
#define LWIP_PLATFORM_ASSERT(x)     do { nnet_port_log("lwip assertion \"%s\" at %s:%d\n", x, __FILE__, __LINE__); } while (0)

#define LWIP_ERR_T int

#endif
