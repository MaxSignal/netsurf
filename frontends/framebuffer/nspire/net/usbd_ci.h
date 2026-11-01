/*
 * usbd_ci.h -- polled USB device-mode driver for the ChipIdea controller of
 * the TI-Nspire CX (see ci_regs.h).
 *
 * The driver takes the controller from TI-OS (usbd_open) and gives it back
 * (usbd_close): while it owns it, TI-OS's USB interrupt is masked and the
 * calculator disappears from the computer as a TI device and comes back as
 * whatever the function driver (ncm.c) describes. Nothing runs in interrupt
 * context: usbd_poll() and the ring functions are called from the program's
 * own loop, and the controller keeps moving data between calls through the
 * transfer rings primed in advance (the host is NAKed, not dropped, when a
 * ring is full or empty, so slow polling only costs bandwidth).
 */
#ifndef USBD_CI_H
#define USBD_CI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct usbd_setup {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usbd_setup;

#define USB_DIR_IN          0x80
#define USB_TYPE_MASK       0x60
#define USB_TYPE_STANDARD   0x00
#define USB_TYPE_CLASS      0x20
#define USB_RECIP_MASK      0x1F
#define USB_RECIP_DEVICE    0x00
#define USB_RECIP_INTERFACE 0x01
#define USB_RECIP_ENDPOINT  0x02

/* What the function (the device's personality) provides. */
typedef struct usbd_function {
    const uint8_t *device_desc;
    const uint8_t *config_desc;
    uint16_t config_len;
    /* String descriptor `index` (bLength/bDescriptorType included) into buf;
     * returns its length, or -1 if there is none. */
    int (*get_string)(uint8_t index, uint8_t *buf, int max);
    /* Class or vendor request. For a device-to-host request, write the reply
     * into buf and return its length; for host-to-device, return 0 (the data
     * stage, if any, arrives through class_out_data). -1 stalls. */
    int (*class_request)(const usbd_setup *s, uint8_t *buf, int max);
    void (*class_out_data)(const usbd_setup *s, const uint8_t *data, int len);
    /* SET_CONFIGURATION (0: back to the address state). */
    void (*set_config)(int config);
    /* SET_INTERFACE; -1 stalls. */
    int (*set_interface)(int intf, int alt);
    int (*get_interface)(int intf);
    /* Bus reset or disconnect: everything the function had going is gone. */
    void (*bus_reset)(void);
} usbd_function;

/* Takes the controller and connects to the host as `fn`. 0 on success. */
int usbd_open(const usbd_function *fn);
/* Disconnects and hands the controller back as TI-OS left it. */
void usbd_close(void);
/* Bus events and the control endpoint. */
void usbd_poll(void);

int usbd_configured(void);
/* "full", "high", "low" or "none" (not reset by a host yet). */
const char *usbd_speed(void);

/* Data endpoints. type: 2 bulk, 3 interrupt. */
void usbd_ep_enable(int num, int in, int type, int maxpkt);
void usbd_ep_disable(int num, int in);

/* A ring of transfers on one endpoint, each with its own buffer.
 * OUT (receive): usbd_ring_rx_fill() primes every free slot;
 *   usbd_ring_rx_poll() hands each completed transfer to `cb` in order and
 *   primes its slot again.
 * IN (send): usbd_ring_tx_buf() gives the next free buffer (or NULL),
 *   usbd_ring_tx_submit() queues it; usbd_ring_tx_reap() frees the slots the
 *   host has read. */
typedef struct usbd_ring usbd_ring;
usbd_ring *usbd_ring_create(int num, int in, int count, int bufsize);
void usbd_ring_reset(usbd_ring *r);           /* flush the hardware, all slots free */
void usbd_ring_rx_fill(usbd_ring *r);
typedef void (*usbd_rx_cb)(void *ctx, const uint8_t *data, int len);
int usbd_ring_rx_poll(usbd_ring *r, usbd_rx_cb cb, void *ctx);
uint8_t *usbd_ring_tx_buf(usbd_ring *r);
void usbd_ring_tx_submit(usbd_ring *r, int len);
int usbd_ring_tx_reap(usbd_ring *r);
int usbd_ring_tx_pending(const usbd_ring *r);
int usbd_ring_bufsize(const usbd_ring *r);

typedef struct usbd_stats {
    uint32_t bus_resets, setups, stalls, rx_xfers, tx_xfers, xfer_errors, reprimes;
} usbd_stats;
const usbd_stats *usbd_get_stats(void);

/* After usbd_close: forget the DMA memory (nnet_port_dma_free_all frees it). */
void usbd_release(void);
void usbd_ring_free(usbd_ring *r);

#ifdef __cplusplus
}
#endif

#endif
