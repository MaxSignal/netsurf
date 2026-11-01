/*
 * nnet_port.h -- what the USB network stack needs from the machine it runs on.
 *
 * The calculator's implementation is nnet_port_device.c: the USB
 * controller's registers at 0xB0000000, the ARM926's data cache, TI-OS's page
 * tables for DMA addresses, the PL190 interrupt controller and the clock.
 *
 * Everything above it is usbd_ci.c, ncm.c and nnet.c.
 */
#ifndef NNET_PORT_H
#define NNET_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* USB controller registers, as offsets from the controller's base. */
uint32_t nnet_port_reg_read(uint32_t offset);
void nnet_port_reg_write(uint32_t offset, uint32_t value);

/* Memory the controller reads and writes by DMA. Never freed while the stack
 * runs; nnet_port_dma_free_all() releases it on shutdown. */
void *nnet_port_dma_alloc(size_t size, size_t align);
void nnet_port_dma_free_all(void);
/* Bus address of a DMA allocation (what goes into dQH/dTD pointers). */
uint32_t nnet_port_v2p(const void *p);
/* Data cache maintenance around DMA: write dirty lines back before the
 * controller reads, drop lines before the CPU reads what the controller
 * wrote. */
void nnet_port_cache_clean(const void *p, size_t n);
void nnet_port_cache_inval(void *p, size_t n);

/* Milliseconds since some point (wraps). */
uint32_t nnet_port_ms(void);
/* Busy wait. */
void nnet_port_udelay(uint32_t us);

/* Keep the operating system's USB interrupt handler away from the controller
 * while the stack owns it (masked = 1), and give it back (masked = 0). */
void nnet_port_os_usb_irq(int masked);

/* Unique-ish bytes for the MAC address when none is configured. */
uint32_t nnet_port_entropy(void);

void nnet_port_log(const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;

#ifdef __cplusplus
}
#endif

#endif
