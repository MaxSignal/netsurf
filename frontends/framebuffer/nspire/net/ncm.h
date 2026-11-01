/*
 * ncm.h -- USB CDC NCM (Network Control Model) function on usbd_ci: the
 * calculator shows up on the computer as a USB Ethernet adapter (Linux
 * cdc_ncm, Windows 10/11 UsbNcm, macOS), and Ethernet frames go both ways in
 * NTB16 transfer blocks over a pair of bulk endpoints.
 */
#ifndef NCM_H
#define NCM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NCM_MAX_FRAME 1514

typedef struct ncm_callbacks {
    void *ctx;
    /* One received Ethernet frame (without FCS). */
    void (*rx_frame)(void *ctx, const uint8_t *frame, int len);
    /* The computer opened (1) or closed (0) the data interface. */
    void (*link)(void *ctx, int up);
} ncm_callbacks;

/* host_mac: the address the computer's side of the link uses (reported in
 * the iMACAddress string). Takes the USB controller. 0 on success. */
int ncm_open(const uint8_t host_mac[6], const ncm_callbacks *cb);
void ncm_close(void);
void ncm_poll(void);
int ncm_link_up(void);

/* Space for one outgoing frame (up to NCM_MAX_FRAME bytes) or NULL while
 * every transmit buffer is still waiting for the computer. */
uint8_t *ncm_tx_begin(void);
void ncm_tx_commit(int len);

typedef struct ncm_stats {
    uint32_t rx_ntbs, rx_frames, rx_bad_ntbs, tx_frames, tx_full, link_ups;
} ncm_stats;
const ncm_stats *ncm_get_stats(void);
/* After ncm_close: drop the buffers (their DMA memory is freed in bulk). */
void ncm_release(void);

#ifdef __cplusplus
}
#endif

#endif
