/*
 * usbd_ci.c -- polled USB device-mode driver for the TI-Nspire CX's ChipIdea
 * controller. See usbd_ci.h for the model and ci_regs.h for the hardware.
 *
 * Cache: the ARM926's data cache is write-back and allocates on read misses
 * only. Descriptors are written whole while the controller does not own them
 * and cleaned to memory; the one field written while the controller may be
 * using a dTD (the next pointer of the last queued one, when appending) is
 * stored after invalidating the line, so the store misses the cache and goes
 * to memory alone instead of a write-back of the whole line over the token
 * the controller may have just updated. Everything the controller writes is
 * invalidated before it is read.
 */
#include "usbd_ci.h"

#include <stdlib.h>
#include <string.h>

#include "ci_regs.h"
#include "nnet_port.h"

#define RD(r)       nnet_port_reg_read(r)
#define WR(r, v)    nnet_port_reg_write((r), (v))

#define NUM_EP      4       /* endpoints used (DCCPARAMS on the CX: 4) */
#define MAX_EP      16      /* the queue head list covers any count the core reports */
#define EP0_MAXPKT  64
#define EP0_BUFSIZE 512

typedef struct saved_regs {
    uint32_t usbcmd, usbintr, usbmode, eplistaddr, portsc, otgsc, deviceaddr, configflag;
    uint32_t endptctrl[MAX_EP];
    int valid;
} saved_regs;

struct usbd_ring {
    int num, in, count, bufsize;
    ci_dtd *td;
    uint8_t *buf;
    int head, tail, inflight;
};

static const usbd_function *g_fn;
static ci_dqh *g_qh;                    /* 2 * MAX_EP queue heads */
static int g_num_ep = NUM_EP;           /* endpoints the core has (DCCPARAMS) */
static int g_addr_pending = -1;         /* SET_ADDRESS waiting for its status stage */
static int g_vbus_off;                  /* detached because VBUS went away */
static ci_dtd *g_ep0_td;                /* [0], [1]: IN data + ZLP; [2]: OUT */
static uint8_t *g_ep0_in_buf, *g_ep0_out_buf;
static saved_regs g_saved;
static int g_open;
static int g_config;
static int g_ep0_out_pending;           /* data stage of a host-to-device request */
static usbd_setup g_ep0_setup;
static int g_speed = -1;
static int g_vbus_seen;                 /* B session valid seen while attached */
static usbd_stats g_stats;

/* ------------------------------------------------------------------------ */

static ci_dqh *qh(int num, int in) { return &g_qh[2 * num + (in ? 1 : 0)]; }

static int wait_clear(uint32_t reg, uint32_t mask, uint32_t timeout_ms)
{
    uint32_t start = nnet_port_ms();
    while (RD(reg) & mask) {
        if ((uint32_t)(nnet_port_ms() - start) > timeout_ms)
            return -1;
    }
    return 0;
}

static void td_fill(ci_dtd *td, void *buf, uint32_t len)
{
    uint32_t pa = buf ? nnet_port_v2p(buf) : 0;
    uintptr_t page = (uintptr_t)buf & ~(uintptr_t)0xFFF;
    td->next = TD_TERMINATE;
    td->token = (len << TD_BYTES_SHIFT) | TD_IOC | TD_ACTIVE;
    td->buf[0] = pa;
    for (int i = 1; i < 5; i++)
        td->buf[i] = buf ? nnet_port_v2p((void *)(page + 0x1000u * (uintptr_t)i)) & ~0xFFFu : 0;
    td->pad = 0;
    nnet_port_cache_clean(td, sizeof(*td));
}

static uint32_t td_token(ci_dtd *td)
{
    nnet_port_cache_inval(td, sizeof(*td));
    return td->token;
}

/* Starts the endpoint on `first` (it must not be priming or running). */
static int prime_direct(int num, int in, ci_dtd *first)
{
    ci_dqh *q = qh(num, in);
    nnet_port_cache_inval(q, sizeof(*q));
    q->next = nnet_port_v2p(first);
    q->token &= ~(TD_ACTIVE | TD_HALTED);
    nnet_port_cache_clean(q, sizeof(*q));
    if (num == 0 && !in && (RD(CI_ENDPTSETUPSTAT) & 1))
        return -1;      /* a new SETUP overrides this stage */
    WR(CI_ENDPTPRIME, EPBIT(num, in));
    if (wait_clear(CI_ENDPTPRIME, EPBIT(num, in), 50) < 0)
        return -1;
    return 0;
}

static void ep_flush(uint32_t bits)
{
    for (int tries = 0; tries < 16; tries++) {
        WR(CI_ENDPTFLUSH, bits);
        wait_clear(CI_ENDPTFLUSH, bits, 50);
        if (!(RD(CI_ENDPTSTAT) & bits))
            break;
    }
}

/* ------------------------------------------------------------------------ */
/* Control endpoint */

static void ep0_stall(void)
{
    WR(CI_ENDPTCTRL(0), RD(CI_ENDPTCTRL(0)) | EPCTRL_TXS | EPCTRL_RXS);
    g_stats.stalls++;
}

/* Data stage to the host, then the status stage from it. */
static void ep0_send(const usbd_setup *s, int len)
{
    if (len > s->wLength)
        len = s->wLength;
    if (len > EP0_BUFSIZE)
        len = EP0_BUFSIZE;
    nnet_port_cache_clean(g_ep0_in_buf, (size_t)len);
    td_fill(&g_ep0_td[0], g_ep0_in_buf, (uint32_t)len);
    /* Shorter than asked for and a multiple of the packet size: a zero
     * length packet ends the data stage. */
    if (len < s->wLength && len > 0 && (len % EP0_MAXPKT) == 0) {
        td_fill(&g_ep0_td[1], NULL, 0);
        g_ep0_td[0].next = nnet_port_v2p(&g_ep0_td[1]);
        nnet_port_cache_clean(&g_ep0_td[0], sizeof(ci_dtd));
    }
    prime_direct(0, 1, &g_ep0_td[0]);
    td_fill(&g_ep0_td[2], NULL, 0);
    prime_direct(0, 0, &g_ep0_td[2]);
}

/* Status stage of a request without data (or after host-to-device data). */
static void ep0_status_in(void)
{
    td_fill(&g_ep0_td[0], NULL, 0);
    prime_direct(0, 1, &g_ep0_td[0]);
}

static void ep0_receive(const usbd_setup *s)
{
    int len = s->wLength > EP0_BUFSIZE ? EP0_BUFSIZE : s->wLength;
    nnet_port_cache_inval(g_ep0_out_buf, EP0_BUFSIZE);
    td_fill(&g_ep0_td[2], g_ep0_out_buf, (uint32_t)len);
    g_ep0_setup = *s;
    g_ep0_out_pending = 1;
    prime_direct(0, 0, &g_ep0_td[2]);
}

static int ep_halted(int num, int in)
{
    uint32_t c = RD(CI_ENDPTCTRL(num));
    return (c & (in ? EPCTRL_TXS : EPCTRL_RXS)) != 0;
}

static void ep_set_halt(int num, int in, int halt)
{
    uint32_t c = RD(CI_ENDPTCTRL(num));
    uint32_t s = in ? EPCTRL_TXS : EPCTRL_RXS;
    uint32_t r = in ? EPCTRL_TXR : EPCTRL_RXR;
    if (halt)
        c |= s;
    else
        c = (c & ~s) | r;   /* unstall and reset the data toggle */
    WR(CI_ENDPTCTRL(num), c);
}

/* Waits up to ms for the IN status stage primed by ep0_status_in(). */
static int wait_status_in(uint32_t ms)
{
    uint32_t start = nnet_port_ms();
    for (;;) {
        (void)RD(CI_ENDPTCOMPLETE);         /* the bus keeps going meanwhile */
        if (!(td_token(&g_ep0_td[0]) & TD_ACTIVE))
            return 1;
        if ((RD(CI_ENDPTSETUPSTAT) & 1) || (uint32_t)(nnet_port_ms() - start) > ms)
            return 0;
    }
}

static void apply_address(void)
{
    if (g_addr_pending < 0)
        return;
    WR(CI_DEVICEADDR, (uint32_t)g_addr_pending << DEVICEADDR_SHIFT);
    g_addr_pending = -1;
}

static int standard_request(const usbd_setup *s)
{
    uint8_t *b = g_ep0_in_buf;
    int recip = s->bmRequestType & USB_RECIP_MASK;
    switch (s->bRequest) {
    case 0x00: /* GET_STATUS */
        b[0] = b[1] = 0;
        if (recip == USB_RECIP_DEVICE)
            b[0] = 1;       /* self-powered (batteries) */
        else if (recip == USB_RECIP_ENDPOINT) {
            if ((s->wIndex & 0x0F) >= NUM_EP)
                return -1;
            b[0] = (uint8_t)ep_halted(s->wIndex & 0x0F, s->wIndex & 0x80);
        }
        ep0_send(s, 2);
        return 0;
    case 0x01: /* CLEAR_FEATURE */
    case 0x03: /* SET_FEATURE */
        if (recip == USB_RECIP_ENDPOINT && s->wValue == 0) {
            int num = s->wIndex & 0x0F;
            if (num >= NUM_EP)
                return -1;
            if (num != 0)
                ep_set_halt(num, s->wIndex & 0x80, s->bRequest == 0x03);
            ep0_status_in();
            return 0;
        }
        return -1;          /* no remote wakeup, no test modes */
    case 0x05: /* SET_ADDRESS */
        /* The new address must take effect after the status stage, which is
         * still sent from address 0. The CX's core is an older ChipIdea
         * (version 0 in its ID register) that may not have USBADRA (Linux
         * never uses it), so wait for the status stage here and set the
         * address plainly; the host sends to it 2 ms later. */
        g_addr_pending = s->wValue & 0x7F;
        ep0_status_in();
        if (wait_status_in(30))
            apply_address();
        return 0;
    case 0x06: { /* GET_DESCRIPTOR */
        int type = s->wValue >> 8, index = s->wValue & 0xFF, len = -1;
        if (type == 1) {
            len = g_fn->device_desc[0];
            memcpy(b, g_fn->device_desc, (size_t)len);
        } else if (type == 2 && index == 0) {
            len = g_fn->config_len > EP0_BUFSIZE ? EP0_BUFSIZE : g_fn->config_len;
            memcpy(b, g_fn->config_desc, (size_t)len);
        } else if (type == 3) {
            len = g_fn->get_string((uint8_t)index, b, EP0_BUFSIZE);
        }
        /* Device qualifier / other speed: a full-speed only device stalls. */
        if (len < 0)
            return -1;
        ep0_send(s, len);
        return 0;
    }
    case 0x08: /* GET_CONFIGURATION */
        b[0] = (uint8_t)g_config;
        ep0_send(s, 1);
        return 0;
    case 0x09: /* SET_CONFIGURATION */
        if (s->wValue > 1)
            return -1;
        g_config = s->wValue;
        g_fn->set_config(g_config);
        ep0_status_in();
        return 0;
    case 0x0A: { /* GET_INTERFACE */
        int alt = g_config ? g_fn->get_interface(s->wIndex) : -1;
        if (alt < 0)
            return -1;
        b[0] = (uint8_t)alt;
        ep0_send(s, 1);
        return 0;
    }
    case 0x0B: /* SET_INTERFACE */
        if (!g_config || g_fn->set_interface(s->wIndex, s->wValue) < 0)
            return -1;
        ep0_status_in();
        return 0;
    }
    return -1;
}

static void handle_setup(void)
{
    uint32_t w0, w1;
    usbd_setup s;

    WR(CI_ENDPTSETUPSTAT, 1);
    do {
        WR(CI_USBCMD, RD(CI_USBCMD) | USBCMD_SUTW);
        nnet_port_cache_inval(qh(0, 0), sizeof(ci_dqh));
        w0 = qh(0, 0)->setup[0];
        w1 = qh(0, 0)->setup[1];
    } while (!(RD(CI_USBCMD) & USBCMD_SUTW));
    WR(CI_USBCMD, RD(CI_USBCMD) & ~USBCMD_SUTW);

    s.bmRequestType = (uint8_t)w0;
    s.bRequest = (uint8_t)(w0 >> 8);
    s.wValue = (uint16_t)(w0 >> 16);
    s.wIndex = (uint16_t)w1;
    s.wLength = (uint16_t)(w1 >> 16);
    g_stats.setups++;

    /* A SETUP ends whatever the last control transfer still had queued. */
    ep_flush(EPBIT(0, 0) | EPBIT(0, 1));
    g_ep0_out_pending = 0;
    g_addr_pending = -1;

    int r;
    if ((s.bmRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD) {
        r = standard_request(&s);
    } else {
        r = g_fn->class_request(&s, g_ep0_in_buf, EP0_BUFSIZE);
        if (r >= 0) {
            if (s.bmRequestType & USB_DIR_IN)
                ep0_send(&s, r);
            else if (s.wLength > EP0_BUFSIZE)
                r = -1;
            else if (s.wLength > 0)
                ep0_receive(&s);
            else
                ep0_status_in();
            if (r >= 0)
                r = 0;
        }
    }
    if (r < 0) {
        ep0_stall();
        nnet_port_log("usb: refused request %02x %02x %04x %04x %04x\n", s.bmRequestType, s.bRequest,
                      s.wValue, s.wIndex, s.wLength);
    }
}

static void ep0_complete(uint32_t done)
{
    if ((done & EPBIT(0, 1)) && g_addr_pending >= 0 && !(td_token(&g_ep0_td[0]) & TD_ACTIVE))
        apply_address();
    if ((done & EPBIT(0, 0)) && g_ep0_out_pending) {
        uint32_t tok = td_token(&g_ep0_td[2]);
        if (!(tok & TD_ACTIVE)) {
            int want = g_ep0_setup.wLength > EP0_BUFSIZE ? EP0_BUFSIZE : g_ep0_setup.wLength;
            int got = want - (int)((tok >> TD_BYTES_SHIFT) & TD_BYTES_MASK);
            g_ep0_out_pending = 0;
            nnet_port_cache_inval(g_ep0_out_buf, EP0_BUFSIZE);
            g_fn->class_out_data(&g_ep0_setup, g_ep0_out_buf, got);
            ep0_status_in();
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Bus */

static void bus_reset(void)
{
    /* Polled, the reset is often seen after it ended and the host's first
     * SETUP has already arrived: keep that one (only a SETUP from before the
     * reset, seen while it is still going on, is stale). */
    if (RD(CI_PORTSC) & PORTSC_PR)
        WR(CI_ENDPTSETUPSTAT, RD(CI_ENDPTSETUPSTAT));
    WR(CI_ENDPTCOMPLETE, RD(CI_ENDPTCOMPLETE));
    wait_clear(CI_ENDPTPRIME, 0xFFFFFFFFu, 100);
    ep_flush(0xFFFFFFFFu);
    WR(CI_DEVICEADDR, 0);
    for (int n = 1; n < g_num_ep; n++)
        WR(CI_ENDPTCTRL(n), 0);
    g_config = 0;
    g_ep0_out_pending = 0;
    g_addr_pending = -1;
    g_stats.bus_resets++;
    if (g_fn && g_fn->bus_reset)
        g_fn->bus_reset();
}

static void init_ep0_qh(void)
{
    memset(g_qh, 0, sizeof(ci_dqh) * 2 * MAX_EP);
    qh(0, 0)->cap = (EP0_MAXPKT << QH_MAXPKT_SHIFT) | QH_IOS | QH_ZLT;
    qh(0, 1)->cap = (EP0_MAXPKT << QH_MAXPKT_SHIFT) | QH_ZLT;
    for (int i = 0; i < 2 * MAX_EP; i++)
        g_qh[i].next = TD_TERMINATE;
    nnet_port_cache_clean(g_qh, sizeof(ci_dqh) * 2 * MAX_EP);
}

int usbd_open(const usbd_function *fn)
{
    if (g_open)
        return 0;
    g_fn = fn;
    memset(&g_stats, 0, sizeof(g_stats));
    if (!g_qh) {
        g_qh = (ci_dqh *)nnet_port_dma_alloc(sizeof(ci_dqh) * 2 * MAX_EP, 2048);
        g_ep0_td = (ci_dtd *)nnet_port_dma_alloc(sizeof(ci_dtd) * 3, 32);
        g_ep0_in_buf = (uint8_t *)nnet_port_dma_alloc(EP0_BUFSIZE, 32);
        g_ep0_out_buf = (uint8_t *)nnet_port_dma_alloc(EP0_BUFSIZE, 32);
        if (!g_qh || !g_ep0_td || !g_ep0_in_buf || !g_ep0_out_buf)
            return -1;
    }

    uint32_t id = RD(CI_ID);
    if ((id & 0x3F) != 0x05) {
        nnet_port_log("usb: no ChipIdea controller (ID %08lx)\n", (unsigned long)id);
        return -1;
    }
    if ((RD(CI_USBMODE) & USBMODE_CM_MASK) == 3) {
        /* Host mode: an OTG cable to another calculator or a device. */
        nnet_port_log("usb: the controller is in host mode (OTG cable), not taking it\n");
        return -1;
    }
    g_num_ep = (int)(RD(CI_DCCPARAMS) & 0x1F);
    if (g_num_ep < NUM_EP || g_num_ep > MAX_EP)
        g_num_ep = g_num_ep < NUM_EP ? NUM_EP : MAX_EP;

    /* What TI-OS had, to give back on close. */
    g_saved.usbcmd = RD(CI_USBCMD);
    g_saved.usbintr = RD(CI_USBINTR);
    g_saved.usbmode = RD(CI_USBMODE);
    g_saved.eplistaddr = RD(CI_ENDPTLISTADDR);
    g_saved.portsc = RD(CI_PORTSC);
    g_saved.otgsc = RD(CI_OTGSC);
    g_saved.deviceaddr = RD(CI_DEVICEADDR);
    g_saved.configflag = RD(0x180);
    for (int n = 0; n < g_num_ep; n++)
        g_saved.endptctrl[n] = RD(CI_ENDPTCTRL(n));
    g_saved.valid = 1;
    nnet_port_log("usb: taking over (ID %08lx, %d endpoints): cmd %08lx intr %08lx mode %08lx list %08lx port %08lx otg %08lx\n",
                  (unsigned long)id, g_num_ep,
                  (unsigned long)g_saved.usbcmd, (unsigned long)g_saved.usbintr,
                  (unsigned long)g_saved.usbmode, (unsigned long)g_saved.eplistaddr,
                  (unsigned long)g_saved.portsc, (unsigned long)g_saved.otgsc);

    nnet_port_os_usb_irq(1);
    WR(CI_USBINTR, 0);

    /* Detach from the computer: it sees the TI device unplugged. */
    WR(CI_USBCMD, RD(CI_USBCMD) & ~USBCMD_RS);
    nnet_port_udelay(20000);

    if ((RD(CI_USBMODE) & USBMODE_CM_MASK) != USBMODE_CM_DEVICE) {
        /* Idle: the mode only changes after a controller reset. (In device
         * mode TI-OS's USBMODE is kept: the driver works with setup lockouts
         * on or off.) */
        WR(CI_USBCMD, USBCMD_RST);
        wait_clear(CI_USBCMD, USBCMD_RST, 200);
        WR(CI_OTGSC, g_saved.otgsc & 0x7F00003Fu);
        WR(CI_USBMODE, USBMODE_CM_DEVICE | USBMODE_SLOM);
    }

    WR(CI_ENDPTSETUPSTAT, RD(CI_ENDPTSETUPSTAT));
    WR(CI_ENDPTCOMPLETE, RD(CI_ENDPTCOMPLETE));
    ep_flush(0xFFFFFFFFu);
    for (int n = 1; n < g_num_ep; n++)
        WR(CI_ENDPTCTRL(n), 0);
    WR(CI_DEVICEADDR, 0);
    WR(CI_USBSTS, 0xFFFFFFFFu);

    /* Full speed, as Linux drives this controller; PHY out of low power. */
    uint32_t port = RD(CI_PORTSC) & ~PORTSC_W1C;
    WR(CI_PORTSC, (port & ~PORTSC_PHCD) | PORTSC_PFSC);

    init_ep0_qh();
    WR(CI_ENDPTLISTADDR, nnet_port_v2p(g_qh));
    g_config = 0;
    g_ep0_out_pending = 0;
    g_addr_pending = -1;
    g_speed = -1;
    g_vbus_seen = 0;
    g_vbus_off = 0;
    g_open = 1;

    /* Give the computer time to notice the TI device left before a new
     * device appears on the same port. */
    nnet_port_udelay(300000);
    WR(CI_USBCMD, (RD(CI_USBCMD) & ~(USBCMD_ITC_MASK | USBCMD_SUTW | USBCMD_ATDTW)) | USBCMD_RS);
    nnet_port_log("usb: attached as a network device\n");
    return 0;
}

void usbd_close(void)
{
    if (!g_open)
        return;
    WR(CI_USBCMD, RD(CI_USBCMD) & ~USBCMD_RS);
    nnet_port_udelay(20000);
    WR(CI_ENDPTSETUPSTAT, RD(CI_ENDPTSETUPSTAT));
    WR(CI_ENDPTCOMPLETE, RD(CI_ENDPTCOMPLETE));
    ep_flush(0xFFFFFFFFu);
    for (int n = 1; n < g_num_ep; n++)
        WR(CI_ENDPTCTRL(n), 0);
    WR(CI_DEVICEADDR, 0);
    g_open = 0;
    g_config = 0;
    if (g_fn && g_fn->bus_reset)
        g_fn->bus_reset();

    if (g_saved.valid) {
        uint32_t cmd = g_saved.usbcmd & ~(USBCMD_RST | USBCMD_SUTW | USBCMD_ATDTW);
        if ((g_saved.usbmode & USBMODE_CM_MASK) != USBMODE_CM_DEVICE) {
            /* It was idle: reset it back to that, with what TI-OS had set. */
            WR(CI_USBCMD, USBCMD_RST);
            wait_clear(CI_USBCMD, USBCMD_RST, 200);
            if (g_saved.usbmode & USBMODE_CM_MASK)
                WR(CI_USBMODE, g_saved.usbmode);
            WR(CI_OTGSC, g_saved.otgsc & 0x7F00003Fu);
            WR(CI_DEVICEADDR, g_saved.deviceaddr);
            WR(0x180, g_saved.configflag);
            cmd &= ~((1u << 4) | (1u << 5));     /* no host schedules */
        } else {
            /* TI-OS's endpoints as it set them (data toggles reset). */
            for (int n = 1; n < g_num_ep; n++) {
                uint32_t c = g_saved.endptctrl[n];
                if (c & EPCTRL_TXE)
                    c |= EPCTRL_TXR;
                if (c & EPCTRL_RXE)
                    c |= EPCTRL_RXR;
                WR(CI_ENDPTCTRL(n), c & ~(EPCTRL_TXS | EPCTRL_RXS));
            }
        }
        uint32_t port = RD(CI_PORTSC) & ~PORTSC_W1C;
        WR(CI_PORTSC, (port & ~PORTSC_PFSC) | (g_saved.portsc & PORTSC_PFSC));
        WR(CI_ENDPTLISTADDR, g_saved.eplistaddr);
        WR(CI_USBSTS, 0xFFFFFFFFu);
        WR(CI_USBINTR, g_saved.usbintr);
        nnet_port_os_usb_irq(0);
        /* Gone long enough for the computer to drop the network device, then
         * TI-OS's device comes back: the computer resets the bus and
         * enumerates it again, which TI-OS handles as a fresh connection. */
        nnet_port_udelay(300000);
        WR(CI_USBCMD, cmd);
        g_saved.valid = 0;
    } else {
        nnet_port_os_usb_irq(0);
    }
    nnet_port_log("usb: controller handed back to the OS\n");
}

void usbd_poll(void)
{
    if (!g_open)
        return;
    /* Cable pulled: VBUS gone (the B session valid comparator, which TI-OS
     * itself watches). Only a change from a level seen is trusted. */
    uint32_t otg = RD(CI_OTGSC);
    if (otg & OTGSC_BSV) {
        g_vbus_seen = 1;
        if (g_vbus_off) {
            /* Plugged in again: pull up D+ for the computer. */
            g_vbus_off = 0;
            WR(CI_USBCMD, RD(CI_USBCMD) | USBCMD_RS);
            nnet_port_log("usb: cable plugged in\n");
        }
    } else if (g_vbus_seen) {
        g_vbus_seen = 0;
        g_vbus_off = 1;
        nnet_port_log("usb: cable unplugged\n");
        WR(CI_USBCMD, RD(CI_USBCMD) & ~USBCMD_RS);   /* no pull-up without VBUS */
        bus_reset();
        g_speed = -1;
    }
    uint32_t sts = RD(CI_USBSTS);
    if (sts)
        WR(CI_USBSTS, sts);
    if (sts & USBSTS_URI)
        bus_reset();
    if (sts & (USBSTS_PCI | USBSTS_URI)) {
        uint32_t port = RD(CI_PORTSC);
        if (!(port & PORTSC_PR)) {
            int spd = (int)((port >> PORTSC_PSPD_SHIFT) & 3);
            if (spd != g_speed) {
                g_speed = spd;
                nnet_port_log("usb: bus at %s speed\n", usbd_speed());
            }
        }
    }
    for (int guard = 0; guard < 4 && (RD(CI_ENDPTSETUPSTAT) & 1); guard++)
        handle_setup();
    uint32_t done = RD(CI_ENDPTCOMPLETE);
    if (done) {
        WR(CI_ENDPTCOMPLETE, done);
        if (done & (EPBIT(0, 0) | EPBIT(0, 1)))
            ep0_complete(done);
    }
}

int usbd_configured(void) { return g_open && g_config != 0; }

const char *usbd_speed(void)
{
    switch (g_speed) {
    case 0: return "full";
    case 1: return "low";
    case 2: return "high";
    default: return "none";
    }
}

const usbd_stats *usbd_get_stats(void) { return &g_stats; }

/* ------------------------------------------------------------------------ */
/* Data endpoints */

void usbd_ep_enable(int num, int in, int type, int maxpkt)
{
    if (num <= 0 || num >= NUM_EP)
        return;
    ci_dqh *q = qh(num, in);
    memset((void *)q, 0, sizeof(*q));
    q->cap = ((uint32_t)maxpkt << QH_MAXPKT_SHIFT) | QH_ZLT;
    q->next = TD_TERMINATE;
    nnet_port_cache_clean(q, sizeof(*q));

    uint32_t c = RD(CI_ENDPTCTRL(num));
    if (in) {
        c &= ~(EPCTRL_TXS | (3u << EPCTRL_TXT_SHIFT));
        c |= ((uint32_t)type << EPCTRL_TXT_SHIFT) | EPCTRL_TXR | EPCTRL_TXE;
        /* The other direction must not stay "control" while unused. */
        if (!(c & EPCTRL_RXE))
            c = (c & ~(3u << EPCTRL_RXT_SHIFT)) | (EPTYPE_BULK << EPCTRL_RXT_SHIFT);
    } else {
        c &= ~(EPCTRL_RXS | (3u << EPCTRL_RXT_SHIFT));
        c |= ((uint32_t)type << EPCTRL_RXT_SHIFT) | EPCTRL_RXR | EPCTRL_RXE;
        if (!(c & EPCTRL_TXE))
            c = (c & ~(3u << EPCTRL_TXT_SHIFT)) | (EPTYPE_BULK << EPCTRL_TXT_SHIFT);
    }
    WR(CI_ENDPTCTRL(num), c);
}

void usbd_ep_disable(int num, int in)
{
    if (num <= 0 || num >= NUM_EP)
        return;
    ep_flush(EPBIT(num, in));
    uint32_t c = RD(CI_ENDPTCTRL(num));
    c &= in ? ~EPCTRL_TXE : ~EPCTRL_RXE;
    WR(CI_ENDPTCTRL(num), c);
}

usbd_ring *usbd_ring_create(int num, int in, int count, int bufsize)
{
    usbd_ring *r = (usbd_ring *)calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    bufsize = (bufsize + 31) & ~31;
    r->num = num;
    r->in = in;
    r->count = count;
    r->bufsize = bufsize;
    r->td = (ci_dtd *)nnet_port_dma_alloc(sizeof(ci_dtd) * (size_t)count, 32);
    /* Buffers of at most 4 KB aligned to their size never cross more pages
     * than a dTD can point at. */
    size_t align = 32;
    while (align < (size_t)bufsize && align < 4096)
        align <<= 1;
    r->buf = (uint8_t *)nnet_port_dma_alloc((size_t)bufsize * (size_t)count, align);
    if (!r->td || !r->buf) {
        free(r);
        return NULL;
    }
    usbd_ring_reset(r);
    return r;
}

void usbd_ring_reset(usbd_ring *r)
{
    if (g_open)
        ep_flush(EPBIT(r->num, r->in));
    r->head = r->tail = r->inflight = 0;
    for (int i = 0; i < r->count; i++) {
        r->td[i].next = TD_TERMINATE;
        r->td[i].token = 0;
    }
    nnet_port_cache_clean(r->td, sizeof(ci_dtd) * (size_t)r->count);
}

int usbd_ring_bufsize(const usbd_ring *r) { return r->bufsize; }

/* Queues slot r->tail behind whatever the endpoint is working on. */
static void ring_append(usbd_ring *r, int len)
{
    int slot = r->tail;
    ci_dtd *td = &r->td[slot];
    uint8_t *buf = r->buf + (size_t)slot * (size_t)r->bufsize;
    uint32_t bit = EPBIT(r->num, r->in);

    td_fill(td, buf, (uint32_t)len);
    r->tail = (slot + 1) % r->count;
    r->inflight++;

    if (r->inflight > 1) {
        ci_dtd *prev = &r->td[(slot + r->count - 1) % r->count];
        /* Store the link alone (see the top of the file). */
        nnet_port_cache_inval(prev, sizeof(*prev));
        prev->next = nnet_port_v2p(td);
        nnet_port_cache_clean(prev, 0);     /* drain the write buffer */
        if (RD(CI_ENDPTPRIME) & bit)
            return;
        uint32_t stat;
        do {
            WR(CI_USBCMD, RD(CI_USBCMD) | USBCMD_ATDTW);
            stat = RD(CI_ENDPTSTAT) & bit;
        } while (!(RD(CI_USBCMD) & USBCMD_ATDTW));
        WR(CI_USBCMD, RD(CI_USBCMD) & ~USBCMD_ATDTW);
        if (stat)
            return;     /* the controller follows the link */
        if (td_token(prev) & TD_ACTIVE)
            return;     /* not reached yet: it will be */
    }
    if (prime_direct(r->num, r->in, td) < 0)
        g_stats.reprimes++;
}

void usbd_ring_rx_fill(usbd_ring *r)
{
    if (!g_open)
        return;
    while (r->inflight < r->count)
        ring_append(r, r->bufsize);
}

int usbd_ring_rx_poll(usbd_ring *r, usbd_rx_cb cb, void *ctx)
{
    int n = 0;
    while (r->inflight > 0) {
        int slot = r->head;
        uint32_t tok = td_token(&r->td[slot]);
        if (tok & TD_ACTIVE)
            break;
        r->head = (slot + 1) % r->count;
        r->inflight--;
        uint8_t *buf = r->buf + (size_t)slot * (size_t)r->bufsize;
        if (tok & (TD_HALTED | TD_DBERR | TD_TRERR)) {
            g_stats.xfer_errors++;
        } else {
            int len = r->bufsize - (int)((tok >> TD_BYTES_SHIFT) & TD_BYTES_MASK);
            nnet_port_cache_inval(buf, (size_t)r->bufsize);
            g_stats.rx_xfers++;
            if (cb)
                cb(ctx, buf, len);
        }
        n++;
        ring_append(r, r->bufsize);
    }
    /* The controller stops on an error; start it again on what is queued. */
    if (r->inflight > 0 && !(RD(CI_ENDPTSTAT) & EPBIT(r->num, r->in)) &&
        !(RD(CI_ENDPTPRIME) & EPBIT(r->num, r->in))) {
        uint32_t tok = td_token(&r->td[r->head]);
        if ((tok & TD_ACTIVE) && !(tok & TD_HALTED)) {
            prime_direct(r->num, r->in, &r->td[r->head]);
            g_stats.reprimes++;
        }
    }
    return n;
}

uint8_t *usbd_ring_tx_buf(usbd_ring *r)
{
    if (!g_open || r->inflight >= r->count)
        return NULL;
    return r->buf + (size_t)r->tail * (size_t)r->bufsize;
}

void usbd_ring_tx_submit(usbd_ring *r, int len)
{
    if (len > r->bufsize)
        len = r->bufsize;
    nnet_port_cache_clean(r->buf + (size_t)r->tail * (size_t)r->bufsize, (size_t)len);
    ring_append(r, len);
    g_stats.tx_xfers++;
}

int usbd_ring_tx_reap(usbd_ring *r)
{
    int n = 0;
    while (r->inflight > 0) {
        uint32_t tok = td_token(&r->td[r->head]);
        if (tok & TD_ACTIVE)
            break;
        if (tok & (TD_HALTED | TD_DBERR | TD_TRERR))
            g_stats.xfer_errors++;
        r->head = (r->head + 1) % r->count;
        r->inflight--;
        n++;
    }
    if (r->inflight > 0 && !(RD(CI_ENDPTSTAT) & EPBIT(r->num, r->in)) &&
        !(RD(CI_ENDPTPRIME) & EPBIT(r->num, r->in))) {
        uint32_t tok = td_token(&r->td[r->head]);
        if ((tok & TD_ACTIVE) && !(tok & TD_HALTED)) {
            prime_direct(r->num, r->in, &r->td[r->head]);
            g_stats.reprimes++;
        }
    }
    return n;
}

int usbd_ring_tx_pending(const usbd_ring *r) { return r->inflight; }

void usbd_ring_free(usbd_ring *r)
{
    free(r);
}

void usbd_release(void)
{
    g_qh = NULL;
    g_ep0_td = NULL;
    g_ep0_in_buf = g_ep0_out_buf = NULL;
}
