/*
 * ncm.c -- USB CDC NCM 1.0 device function. See ncm.h.
 *
 * Layout (as Linux's f_ncm, so every host driver that knows that gadget
 * binds): an interface association of a communication interface (class 2,
 * subclass 0x0D, one interrupt IN endpoint for notifications) and a data
 * interface (class 0x0A, protocol 1) whose alternate setting 1 has the bulk
 * IN/OUT pair. The device class is "miscellaneous / IAD" so Windows binds
 * its composite driver and then UsbNcm to the function.
 *
 * NTBs: NTB16 only, no CRC. Each NTB the calculator sends carries one
 * datagram (12 byte NTH16, 16 byte NDP16, the frame at offset 28) and is
 * never a multiple of the packet size, so no zero length packet is needed.
 * Received NTBs may carry any number of datagrams and NDPs.
 */
#include "ncm.h"

#include <string.h>

#include "nnet_port.h"
#include "usbd_ci.h"

#define VID             0x0525      /* NetChip: Linux's "Ethernet Gadget" ids */
#define PID             0xA4A1
#define EP_DATA         1
#define EP_NOTIFY       2
#define BULK_MAXPKT     64
#define NOTIFY_MAXPKT   16
#define NTB_SIZE        2048        /* the minimum NCM lets either side use */
#define RX_SLOTS        16
#define TX_SLOTS        8
#define TX_HEADER       28

#define NTH16_SIG       0x484D434Eu /* "NCMH" */
#define NDP16_SIG       0x304D434Eu /* "NCM0" */
#define MS_VENDOR_CODE  0x4D        /* answers Windows' OS descriptor queries */

static const uint8_t g_device_desc[18] = {
    18, 1, 0x00, 0x02,          /* USB 2.0 */
    0xEF, 0x02, 0x01,           /* miscellaneous, interface association */
    64,
    VID & 0xFF, VID >> 8, PID & 0xFF, PID >> 8,
    0x01, 0x01,                 /* bcdDevice 1.01 (Windows caches OS descriptors per version) */
    1, 2, 3,                    /* manufacturer, product, serial */
    1,
};

#define CONFIG_LEN 94
static const uint8_t g_config_desc[CONFIG_LEN] = {
    9, 2, CONFIG_LEN, 0, 2, 1, 0, 0xC0, 50,     /* self-powered, 100 mA */
    /* interface association: interfaces 0-1, CDC NCM */
    8, 0x0B, 0, 2, 0x02, 0x0D, 0x00, 2,
    /* interface 0: communication, NCM */
    9, 4, 0, 0, 1, 0x02, 0x0D, 0x00, 0,
    5, 0x24, 0x00, 0x10, 0x01,                  /* CDC header 1.10 */
    5, 0x24, 0x06, 0, 1,                        /* union: 0 controls 1 */
    13, 0x24, 0x0F, 4, 0, 0, 0, 0,              /* Ethernet: MAC in string 4 */
        NCM_MAX_FRAME & 0xFF, NCM_MAX_FRAME >> 8, 0, 0, 0,
    6, 0x24, 0x1A, 0x00, 0x01, 0x00,            /* NCM 1.00, no optional requests */
    7, 5, 0x80 | EP_NOTIFY, 3, NOTIFY_MAXPKT, 0, 32,
    /* interface 1: data, alternate 0 without endpoints */
    9, 4, 1, 0, 0, 0x0A, 0x00, 0x01, 0,
    /* interface 1, alternate 1: the bulk pair */
    9, 4, 1, 1, 2, 0x0A, 0x00, 0x01, 0,
    7, 5, 0x80 | EP_DATA, 2, BULK_MAXPKT, 0, 0,
    7, 5, EP_DATA, 2, BULK_MAXPKT, 0, 0,
};

static ncm_callbacks g_cb;
static uint8_t g_host_mac[6];
static usbd_ring *g_rx, *g_tx, *g_notify;
static int g_alt;               /* data interface alternate setting */
static int g_link;
static uint16_t g_tx_seq;
static uint32_t g_ntb_in_max = NTB_SIZE;
static int g_notify_state;      /* 0 nothing, 1 speed next, 2 connect next */
static ncm_stats g_stats;
static uint8_t g_acc[2 * NTB_SIZE];     /* received NTB stream, see parse_ntb */
static uint32_t g_acc_len;

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (get16(p + 2) << 16); }

static int string_desc(const char *ascii, uint8_t *buf, int max)
{
    int n = (int)strlen(ascii);
    int len = 2 + 2 * n;
    if (len > max)
        len = max & ~1;
    buf[0] = (uint8_t)len;
    buf[1] = 3;
    for (int i = 0; 2 + 2 * i < len; i++) {
        buf[2 + 2 * i] = (uint8_t)ascii[i];
        buf[3 + 2 * i] = 0;
    }
    return len;
}

static void mac_hex(const uint8_t *mac, char *out)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) {
        out[2 * i] = hex[mac[i] >> 4];
        out[2 * i + 1] = hex[mac[i] & 15];
    }
    out[12] = 0;
}

static int get_string(uint8_t index, uint8_t *buf, int max)
{
    char text[13];
    switch (index) {
    case 0:
        buf[0] = 4; buf[1] = 3; buf[2] = 0x09; buf[3] = 0x04;  /* English (US) */
        return 4;
    case 1: return string_desc("TI-Nspire homebrew", buf, max);
    case 2: return string_desc("NetSurf USB network (CDC NCM)", buf, max);
    case 3: {
        uint8_t serial[6];
        memcpy(serial, g_host_mac, 6);
        serial[5] ^= 1;
        mac_hex(serial, text);
        return string_desc(text, buf, max);
    }
    case 4:
        mac_hex(g_host_mac, text);
        return string_desc(text, buf, max);
    case 0xEE: {
        /* Microsoft OS string descriptor: Windows then asks (vendor request
         * MS_VENDOR_CODE) for the compatible ID of the function, "WINNCM",
         * which its UsbNcm driver binds to. */
        int len = string_desc("MSFT100", buf, max);
        if (len + 2 > max)
            return -1;
        buf[len] = MS_VENDOR_CODE;
        buf[len + 1] = 0;
        buf[0] = (uint8_t)(len + 2);
        return len + 2;
    }
    }
    return -1;
}

static int class_request(const usbd_setup *s, uint8_t *b, int max)
{
    (void)max;
    if ((s->bmRequestType & USB_TYPE_MASK) == 0x40) {
        /* Microsoft OS 1.0 extended compat ID descriptor. */
        if (s->bRequest != MS_VENDOR_CODE || s->wIndex != 0x0004 || !(s->bmRequestType & USB_DIR_IN))
            return -1;
        memset(b, 0, 40);
        put32(b + 0, 40);
        put16(b + 4, 0x0100);
        put16(b + 6, 0x0004);
        b[8] = 1;                       /* one function section */
        b[16] = 0;                      /* first interface of the function */
        b[17] = 1;
        memcpy(b + 18, "WINNCM", 6);
        return 40;
    }
    if ((s->bmRequestType & USB_RECIP_MASK) != USB_RECIP_INTERFACE || (s->wIndex & 0xFF) != 0)
        return -1;
    switch (s->bRequest) {
    case 0x80: /* GET_NTB_PARAMETERS */
        memset(b, 0, 28);
        put16(b + 0, 28);
        put16(b + 2, 0x0001);           /* NTB16 */
        put32(b + 4, NTB_SIZE);         /* dwNtbInMaxSize */
        put16(b + 8, 4);                /* wNdpInDivisor */
        put16(b + 10, 0);
        put16(b + 12, 4);               /* wNdpInAlignment */
        put32(b + 16, NTB_SIZE);        /* dwNtbOutMaxSize */
        put16(b + 20, 4);               /* wNdpOutDivisor */
        put16(b + 22, 0);
        put16(b + 24, 4);               /* wNdpOutAlignment */
        put16(b + 26, 0);               /* wNtbOutMaxDatagrams: no limit */
        return 28;
    case 0x83: /* GET_NTB_FORMAT */
        put16(b, 0);
        return 2;
    case 0x84: /* SET_NTB_FORMAT */
        return s->wValue == 0 ? 0 : -1;
    case 0x85: /* GET_NTB_INPUT_SIZE */
        put32(b, g_ntb_in_max);
        return 4;
    case 0x86: /* SET_NTB_INPUT_SIZE: value in the data stage */
        return 0;
    case 0x87: /* GET_MAX_DATAGRAM_SIZE */
        put16(b, NCM_MAX_FRAME);
        return 2;
    case 0x88: /* SET_MAX_DATAGRAM_SIZE */
        return 0;
    case 0x89: /* GET_CRC_MODE */
        put16(b, 0);
        return 2;
    case 0x8A: /* SET_CRC_MODE */
        return s->wValue == 0 ? 0 : -1;
    case 0x40: /* SET_ETHERNET_MULTICAST_FILTERS */
    case 0x43: /* SET_ETHERNET_PACKET_FILTER */
        return 0;
    }
    return -1;
}

static void class_out_data(const usbd_setup *s, const uint8_t *data, int len)
{
    if (s->bRequest == 0x86 && len >= 4) {
        uint32_t v = get32(data);
        g_ntb_in_max = v < NTB_SIZE ? v : NTB_SIZE;
        if (g_ntb_in_max < 64)
            g_ntb_in_max = NTB_SIZE;
    }
}

static void set_link(int up)
{
    if (up == g_link)
        return;
    g_link = up;
    if (up)
        g_stats.link_ups++;
    nnet_port_log("ncm: link %s\n", up ? "up" : "down");
    if (g_cb.link)
        g_cb.link(g_cb.ctx, up);
}

static void data_off(void)
{
    g_acc_len = 0;
    if (g_rx) {
        usbd_ring_reset(g_rx);
        usbd_ring_reset(g_tx);
    }
    usbd_ep_disable(EP_DATA, 0);
    usbd_ep_disable(EP_DATA, 1);
    g_alt = 0;
    set_link(0);
}

static void set_config(int config)
{
    data_off();
    nnet_port_log("ncm: configuration %d\n", config);
    if (config) {
        usbd_ep_enable(EP_NOTIFY, 1, 3, NOTIFY_MAXPKT);
        usbd_ring_reset(g_notify);
        /* Report the link at once, as a USB Ethernet adapter reports its
         * cable: macOS opens the data interface (alternate 1) only after
         * the device said it is connected. Linux and Windows take it either
         * way; it is said again when the data interface opens. */
        g_notify_state = 1;
    } else {
        usbd_ep_disable(EP_NOTIFY, 1);
        g_notify_state = 0;
    }
}

static int set_interface(int intf, int alt)
{
    if (intf == 0)
        return alt == 0 ? 0 : -1;
    if (intf != 1 || alt > 1)
        return -1;
    nnet_port_log("ncm: data interface alternate %d\n", alt);
    data_off();
    if (alt == 1) {
        usbd_ep_enable(EP_DATA, 0, 2, BULK_MAXPKT);
        usbd_ep_enable(EP_DATA, 1, 2, BULK_MAXPKT);
        usbd_ring_reset(g_rx);
        usbd_ring_reset(g_tx);
        usbd_ring_rx_fill(g_rx);
        g_tx_seq = 0;
        g_alt = 1;
        g_notify_state = 1;
        set_link(1);
    }
    return 0;
}

static int get_interface(int intf)
{
    if (intf == 0)
        return 0;
    if (intf == 1)
        return g_alt;
    return -1;
}

static void bus_reset(void)
{
    g_alt = 0;
    g_notify_state = 0;
    g_ntb_in_max = NTB_SIZE;
    if (g_rx) {
        usbd_ring_reset(g_rx);
        usbd_ring_reset(g_tx);
        usbd_ring_reset(g_notify);
    }
    set_link(0);
}

static const usbd_function g_function = {
    g_device_desc, g_config_desc, CONFIG_LEN,
    get_string, class_request, class_out_data,
    set_config, set_interface, get_interface, bus_reset,
};

/* ------------------------------------------------------------------------ */

static void parse_one_ntb(const uint8_t *p, uint32_t block)
{
    g_stats.rx_ntbs++;
    uint32_t ndp = get16(p + 10);
    for (int guard = 0; ndp != 0 && guard < 16; guard++) {
        if ((ndp & 3) || ndp + 16 > block) {
            g_stats.rx_bad_ntbs++;
            return;
        }
        const uint8_t *n = p + ndp;
        uint32_t sig = get32(n);
        uint32_t nlen = get16(n + 4);
        if ((sig != NDP16_SIG && sig != 0x314D434Eu) || nlen < 16 || ndp + nlen > block) {
            g_stats.rx_bad_ntbs++;
            return;
        }
        for (uint32_t e = 8; e + 4 <= nlen; e += 4) {
            uint32_t idx = get16(n + e), dlen = get16(n + e + 2);
            if (idx == 0 || dlen == 0)
                break;
            if (sig == 0x314D434Eu && dlen >= 4)
                dlen -= 4;              /* CRC appended (never asked for) */
            if (idx + dlen > block || dlen < 14 || dlen > NCM_MAX_FRAME) {
                g_stats.rx_bad_ntbs++;
                continue;
            }
            g_stats.rx_frames++;
            if (g_cb.rx_frame)
                g_cb.rx_frame(g_cb.ctx, p + idx, (int)dlen);
        }
        ndp = get16(n + 6);
    }
}

/* Received transfers. Normally one transfer is one NTB (the host ends each
 * with a short packet, or it fills a whole buffer). Hosts that end an NTB
 * that is a multiple of the packet size without a zero length packet (seen
 * with Windows 10's driver) make the controller run NTBs together, across
 * buffers: the data is then taken as a stream and cut by wBlockLength. */

static void parse_ntb(void *ctx, const uint8_t *data, int len)
{
    (void)ctx;
    int short_xfer = len < NTB_SIZE;
    if (g_acc_len + (uint32_t)len > sizeof(g_acc)) {
        g_acc_len = 0;                  /* lost track: start over */
        g_stats.rx_bad_ntbs++;
    }
    memcpy(g_acc + g_acc_len, data, (size_t)len);
    g_acc_len += (uint32_t)len;

    uint32_t off = 0;
    while (g_acc_len - off >= 12) {
        const uint8_t *p = g_acc + off;
        uint32_t block = get16(p + 8);
        if (block == 0 && short_xfer)
            block = g_acc_len - off;    /* "up to the end of the transfer" */
        if (get32(p) != NTH16_SIG || get16(p + 4) != 12 || (block != 0 && block < 12) || block > NTB_SIZE) {
            g_stats.rx_bad_ntbs++;
            off = g_acc_len;            /* not at an NTB: drop the rest */
            break;
        }
        if (block == 0)
            break;                      /* ends with the transfer, still coming */
        if (block > g_acc_len - off)
            break;                      /* the rest is still coming */
        parse_one_ntb(p, block);
        off += block;
        /* A single padding byte may end a transfer: skip what cannot be an
         * NTH before the next one. */
        while (off < g_acc_len && g_acc_len - off < 12 && short_xfer)
            off = g_acc_len;
    }
    if (short_xfer)
        off = g_acc_len;                /* a transfer ended: nothing carries over */
    if (off) {
        memmove(g_acc, g_acc + off, g_acc_len - off);
        g_acc_len -= off;
    }
}

static void notify_poll(void)
{
    usbd_ring_tx_reap(g_notify);
    if (!g_notify_state || usbd_ring_tx_pending(g_notify))
        return;
    uint8_t *b = usbd_ring_tx_buf(g_notify);
    if (!b)
        return;
    if (g_notify_state == 1) {
        /* CONNECTION_SPEED_CHANGE: full speed both ways */
        b[0] = 0xA1; b[1] = 0x2A; put16(b + 2, 0); put16(b + 4, 0); put16(b + 6, 8);
        put32(b + 8, 12000000); put32(b + 12, 12000000);
        usbd_ring_tx_submit(g_notify, 16);
        g_notify_state = 2;
    } else {
        /* NETWORK_CONNECTION: connected */
        b[0] = 0xA1; b[1] = 0x00; put16(b + 2, 1); put16(b + 4, 0); put16(b + 6, 0);
        usbd_ring_tx_submit(g_notify, 8);
        g_notify_state = 0;
    }
}

int ncm_open(const uint8_t host_mac[6], const ncm_callbacks *cb)
{
    memcpy(g_host_mac, host_mac, 6);
    g_cb = *cb;
    memset(&g_stats, 0, sizeof(g_stats));
    if (!g_rx) {
        g_rx = usbd_ring_create(EP_DATA, 0, RX_SLOTS, NTB_SIZE);
        g_tx = usbd_ring_create(EP_DATA, 1, TX_SLOTS, NTB_SIZE);
        g_notify = usbd_ring_create(EP_NOTIFY, 1, 2, 32);
        if (!g_rx || !g_tx || !g_notify)
            return -1;
    }
    g_alt = 0;
    g_link = 0;
    g_notify_state = 0;
    return usbd_open(&g_function);
}

void ncm_close(void)
{
    usbd_close();
    g_alt = 0;
    g_link = 0;
}

void ncm_poll(void)
{
    usbd_poll();
    if (g_alt == 1) {
        usbd_ring_rx_poll(g_rx, parse_ntb, NULL);
        usbd_ring_tx_reap(g_tx);
    }
    if (usbd_configured())
        notify_poll();
}

int ncm_link_up(void) { return g_link; }

uint8_t *ncm_tx_begin(void)
{
    if (g_alt != 1)
        return NULL;
    uint8_t *b = usbd_ring_tx_buf(g_tx);
    if (!b) {
        usbd_ring_tx_reap(g_tx);
        b = usbd_ring_tx_buf(g_tx);
    }
    if (!b) {
        g_stats.tx_full++;
        return NULL;
    }
    return b + TX_HEADER;
}

void ncm_tx_commit(int len)
{
    if (g_alt != 1)
        return;
    uint8_t *b = usbd_ring_tx_buf(g_tx);
    if (!b || len <= 0 || len > NCM_MAX_FRAME)
        return;
    uint32_t total = TX_HEADER + (uint32_t)len;
    /* Never a whole number of packets: the transfer ends on a short packet. */
    if (total % BULK_MAXPKT == 0)
        b[total++] = 0;
    if (total > g_ntb_in_max)
        return;
    put32(b + 0, NTH16_SIG);
    put16(b + 4, 12);
    put16(b + 6, g_tx_seq++);
    put16(b + 8, total);
    put16(b + 10, 12);                  /* NDP right after the header */
    put32(b + 12, NDP16_SIG);
    put16(b + 16, 16);
    put16(b + 18, 0);
    put16(b + 20, TX_HEADER);
    put16(b + 22, (uint32_t)len);
    put32(b + 24, 0);                   /* terminating entry */
    usbd_ring_tx_submit(g_tx, (int)total);
    g_stats.tx_frames++;
}

const ncm_stats *ncm_get_stats(void) { return &g_stats; }

void ncm_release(void)
{
    usbd_ring_free(g_rx);
    usbd_ring_free(g_tx);
    usbd_ring_free(g_notify);
    g_rx = g_tx = g_notify = NULL;
    usbd_release();
}
