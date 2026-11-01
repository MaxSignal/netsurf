/*
 * nnet.c -- lwIP on the USB CDC NCM link. See nnet.h.
 */
#include "nnet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"
#include "netif/ethernet.h"

#include "ncm.h"
#include "nnet_port.h"

static void nnet_dma_release(void)
{
    ncm_release();
    nnet_port_dma_free_all();
}

static nnet_config g_cfg;
static struct netif g_netif;
static int g_lwip_ready;
static int g_running;
static int g_link;
static uint32_t g_link_ms;
static int g_server;                /* we are the link's DHCP server */
static uint8_t g_host_mac[6];       /* the computer's end of the link */
static uint32_t g_pc_seen;          /* an address the computer sent from */
static int g_pc_phase;              /* 0 nothing, 1 DHCP retried, 2 joined its subnet */
static uint32_t g_pc_phase_ms;
static uint32_t g_pc_peer;
static int g_first_rx_logged;
static uint32_t g_rx_frames, g_tx_frames, g_stats_ms;
static int g_addr_logged;
static uint32_t g_server_ip, g_server_peer, g_server_mask;
static struct udp_pcb *g_dhcpd;
static uint32_t g_rand_state = 0x9E3779B9u;
static uint32_t g_rx_drops, g_tx_drops;

uint32_t nnet_rand(void)
{
    uint32_t x = g_rand_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rand_state = x;
    return x;
}

uint32_t sys_now(void) { return nnet_port_ms(); }

void nnet_lwip_diag(const char *fmt, ...)
{
    (void)fmt;
}

/* ------------------------------------------------------------------------ */

void nnet_format_ip(uint32_t ip, char *buf, int size)
{
    snprintf(buf, (size_t)size, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 255),
             (unsigned)((ip >> 8) & 255), (unsigned)(ip & 255));
}

int nnet_parse_ip(const char *s, uint32_t *ip)
{
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9')
            return 0;
        uint32_t n = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (uint32_t)(*s++ - '0');
            if (++digits > 3 || n > 255)
                return 0;
        }
        v = (v << 8) | n;
        if (part < 3) {
            if (*s != '.')
                return 0;
            s++;
        }
    }
    if (*s)
        return 0;
    *ip = v;
    return 1;
}

static void to_lwip(uint32_t host, ip4_addr_t *a) { ip4_addr_set_u32(a, lwip_htonl(host)); }
static uint32_t from_lwip(const ip4_addr_t *a) { return lwip_ntohl(ip4_addr_get_u32(a)); }

void nnet_default_config(nnet_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode = NNET_MODE_AUTO;
    cfg->dhcp_wait_ms = 5000;
    strcpy(cfg->hostname, "nspire");
}

/* ------------------------------------------------------------------------ */
/* NCM <-> lwIP */

static err_t linkoutput(struct netif *n, struct pbuf *p)
{
    (void)n;
    if (!g_link || p->tot_len > NCM_MAX_FRAME) {
        g_tx_drops++;
        return ERR_IF;
    }
    uint8_t *b = ncm_tx_begin();
    uint32_t start = nnet_port_ms();
    /* Every transmit buffer queued: the computer reads them on its own, so
     * waiting a little frees one (USB events are left for nnet_poll). */
    while (!b && (uint32_t)(nnet_port_ms() - start) < 50) {
        nnet_port_udelay(200);
        b = ncm_tx_begin();
    }
    if (!b) {
        g_tx_drops++;
        return ERR_MEM;
    }
    pbuf_copy_partial(p, b, p->tot_len, 0);
    int len = p->tot_len;
    if (len < 60) {
        memset(b + len, 0, (size_t)(60 - len));
        len = 60;
    }
    ncm_tx_commit(len);
    g_tx_frames++;
    return ERR_OK;
}

/* The computer's own IPv4 address, as it shows in what it sends (ARP, mDNS,
 * anything), for when it has one we did not give it. */
static void sniff(const uint8_t *f, int len)
{
    if (len < 34 || memcmp(f + 6, g_host_mac, 6) != 0)
        return;
    uint32_t type = ((uint32_t)f[12] << 8) | f[13], ip = 0;
    if (type == 0x0800)
        ip = ((uint32_t)f[26] << 24) | ((uint32_t)f[27] << 16) | ((uint32_t)f[28] << 8) | f[29];
    else if (type == 0x0806 && len >= 42)
        ip = ((uint32_t)f[28] << 24) | ((uint32_t)f[29] << 16) | ((uint32_t)f[30] << 8) | f[31];
    if (!g_first_rx_logged) {
        char a[16];
        g_first_rx_logged = 1;
        nnet_format_ip(ip, a, sizeof(a));
        nnet_port_log("net: first frame from the computer (type %04lx, from %s)\n", (unsigned long)type, a);
    }
    if (ip != 0 && (ip >> 24) < 224 && (ip >> 24) != 127)
        g_pc_seen = ip;
}

static void rx_frame(void *ctx, const uint8_t *frame, int len)
{
    (void)ctx;
    g_rx_frames++;
    sniff(frame, len);
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
    if (!p) {
        g_rx_drops++;
        return;
    }
    pbuf_take(p, frame, (u16_t)len);
    if (g_netif.input(p, &g_netif) != ERR_OK)
        pbuf_free(p);
}

static void dhcpd_stop(void);
static void address_changed(void);

static void link_cb(void *ctx, int up)
{
    (void)ctx;
    g_link = up;
    g_addr_logged = 0;
    g_pc_seen = 0;
    g_pc_phase = 0;
    g_first_rx_logged = 0;
    if (up) {
        g_link_ms = nnet_port_ms();
        netif_set_link_up(&g_netif);
        address_changed();
    } else {
        dhcp_stop(&g_netif);
        dhcpd_stop();
        g_server = 0;
        netif_set_addr(&g_netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
        netif_set_link_down(&g_netif);
    }
}

static err_t netif_init_cb(struct netif *n)
{
    n->name[0] = 'u';
    n->name[1] = 's';
    n->output = etharp_output;
    n->linkoutput = linkoutput;
    n->mtu = 1500;
    n->hwaddr_len = 6;
    memcpy(n->hwaddr, g_cfg.mac, 6);
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
#if LWIP_NETIF_HOSTNAME
    n->hostname = g_cfg.hostname;
#endif
    return ERR_OK;
}

/* ------------------------------------------------------------------------ */
/* DHCP server for the computer at the other end, when it has none */

static void dhcpd_reply(const uint8_t *req, int type, const struct pbuf *in)
{
    (void)in;
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 300, PBUF_RAM);
    if (!p)
        return;
    uint8_t *o = (uint8_t *)p->payload;
    memset(o, 0, 300);
    o[0] = 2; o[1] = 1; o[2] = 6;
    memcpy(o + 4, req + 4, 4);              /* xid */
    memcpy(o + 10, req + 10, 2);            /* flags */
    uint32_t yi = lwip_htonl(g_server_peer), si = lwip_htonl(g_server_ip);
    if (type != 6)
        memcpy(o + 16, &yi, 4);             /* yiaddr */
    memcpy(o + 20, &si, 4);                 /* siaddr */
    memcpy(o + 28, req + 28, 16);           /* chaddr */
    o[236] = 99; o[237] = 130; o[238] = 83; o[239] = 99;
    uint8_t *q = o + 240;
    uint32_t mask = lwip_htonl(g_server_mask);
    *q++ = 53; *q++ = 1; *q++ = (uint8_t)type;
    *q++ = 54; *q++ = 4; memcpy(q, &si, 4); q += 4;
    if (type != 6) {
        static const uint8_t lease[4] = {0x00, 0x01, 0x51, 0x80};  /* 1 day */
        static const uint8_t t1[4] = {0x00, 0x00, 0xA8, 0xC0};
        static const uint8_t t2[4] = {0x00, 0x01, 0x27, 0x50};
        *q++ = 51; *q++ = 4; memcpy(q, lease, 4); q += 4;
        *q++ = 58; *q++ = 4; memcpy(q, t1, 4); q += 4;
        *q++ = 59; *q++ = 4; memcpy(q, t2, 4); q += 4;
        *q++ = 1; *q++ = 4; memcpy(q, &mask, 4); q += 4;
    }
    *q++ = 255;
    pbuf_realloc(p, (u16_t)(q - o < 300 ? 300 : q - o));
    udp_sendto_if(g_dhcpd, p, IP_ADDR_BROADCAST, 68, &g_netif);
    pbuf_free(p);
}

static void dhcpd_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    (void)arg; (void)pcb; (void)addr; (void)port;
    uint8_t req[548];
    int len = p->tot_len > (int)sizeof(req) ? (int)sizeof(req) : p->tot_len;
    pbuf_copy_partial(p, req, (u16_t)len, 0);
    pbuf_free(p);
    if (len < 244 || req[0] != 1 || req[1] != 1 || req[2] != 6 ||
        req[236] != 99 || req[237] != 130 || req[238] != 83 || req[239] != 99)
        return;
    int type = 0;
    uint32_t requested = 0;
    for (int i = 240; i < len;) {
        int opt = req[i];
        if (opt == 255)
            break;
        if (opt == 0) {
            i++;
            continue;
        }
        if (i + 1 >= len)
            break;
        int olen = req[i + 1];
        if (i + 2 + olen > len)
            break;
        if (opt == 53 && olen >= 1)
            type = req[i + 2];
        if (opt == 50 && olen == 4)
            requested = ((uint32_t)req[i + 2] << 24) | ((uint32_t)req[i + 3] << 16) |
                        ((uint32_t)req[i + 4] << 8) | req[i + 5];
        i += 2 + olen;
    }
    if (type == 1) {                        /* DISCOVER -> OFFER */
        dhcpd_reply(req, 2, NULL);
    } else if (type == 3) {                 /* REQUEST -> ACK, or NAK */
        uint32_t ci = ((uint32_t)req[12] << 24) | ((uint32_t)req[13] << 16) | ((uint32_t)req[14] << 8) | req[15];
        uint32_t want = requested ? requested : ci;
        if (want && want != g_server_peer) {
            dhcpd_reply(req, 6, NULL);
        } else {
            dhcpd_reply(req, 5, NULL);
            char a[16];
            nnet_format_ip(g_server_peer, a, sizeof(a));
            nnet_port_log("net: computer took %s\n", a);
        }
    } else if (type == 8) {                 /* INFORM -> ACK */
        dhcpd_reply(req, 5, NULL);
    }
}

static void dhcpd_start(void)
{
    if (g_dhcpd)
        return;
    g_dhcpd = udp_new();
    if (!g_dhcpd)
        return;
    ip_set_option(g_dhcpd, SOF_BROADCAST);
    if (udp_bind(g_dhcpd, IP_ADDR_ANY, 67) != ERR_OK) {
        udp_remove(g_dhcpd);
        g_dhcpd = NULL;
        return;
    }
    udp_recv(g_dhcpd, dhcpd_recv, NULL);
}

static void dhcpd_stop(void)
{
    if (g_dhcpd) {
        udp_remove(g_dhcpd);
        g_dhcpd = NULL;
    }
}

static void become_server(void)
{
    if (g_cfg.ip && g_cfg.netmask) {
        g_server_ip = g_cfg.ip;
        g_server_mask = g_cfg.netmask;
    } else {
        /* 10.A.B.1/24 from the MAC: different calculators, different subnets. */
        uint32_t a = 200u + (uint32_t)(g_cfg.mac[4] % 50), b = g_cfg.mac[5];
        g_server_ip = (10u << 24) | (a << 16) | (b << 8) | 1u;
        g_server_mask = 0xFFFFFF00u;
    }
    g_server_peer = (g_server_ip & g_server_mask) | ((g_server_ip & ~g_server_mask) == 2 ? 1u : 2u);
    ip4_addr_t ip, mask;
    to_lwip(g_server_ip, &ip);
    to_lwip(g_server_mask, &mask);
    netif_set_addr(&g_netif, &ip, &mask, IP4_ADDR_ANY4);
    g_server = 1;
    dhcpd_start();
    char a[16], b2[16];
    nnet_format_ip(g_server_ip, a, sizeof(a));
    nnet_format_ip(g_server_peer, b2, sizeof(b2));
    nnet_port_log("net: no DHCP from the computer; serving it %s, calculator %s\n", b2, a);
}

/* Link just came up: start the configured way of getting an address. */
static void address_changed(void)
{
    switch (g_cfg.mode) {
    case NNET_MODE_SERVER:
        become_server();
        break;
    case NNET_MODE_STATIC: {
        ip4_addr_t ip, mask, gw;
        to_lwip(g_cfg.ip, &ip);
        to_lwip(g_cfg.netmask ? g_cfg.netmask : 0xFFFFFF00u, &mask);
        to_lwip(g_cfg.gateway, &gw);
        netif_set_addr(&g_netif, &ip, &mask, &gw);
        if (g_cfg.dns) {
            ip_addr_t d;
            ip_addr_set_ip4_u32(&d, lwip_htonl(g_cfg.dns));
            dns_setserver(0, &d);
        }
        break;
    }
    default:
        dhcp_start(&g_netif);
        break;
    }
}

/* ------------------------------------------------------------------------ */

int nnet_start(const nnet_config *cfg)
{
    if (g_running)
        return 0;
    g_cfg = *cfg;
    static const uint8_t zero[6];
    g_rand_state ^= nnet_port_entropy() | 1u;
    if (!memcmp(g_cfg.mac, zero, 6)) {
        uint32_t e = nnet_port_entropy();
        g_cfg.mac[0] = 0x02;                /* locally administered, unicast */
        g_cfg.mac[1] = 0x4E;
        g_cfg.mac[2] = (uint8_t)(e >> 24);
        g_cfg.mac[3] = (uint8_t)(e >> 16);
        g_cfg.mac[4] = (uint8_t)(e >> 8);
        g_cfg.mac[5] = (uint8_t)e;
    }
    g_cfg.mac[0] = (uint8_t)((g_cfg.mac[0] | 0x02) & ~0x01);
    if (!g_cfg.dhcp_wait_ms)
        g_cfg.dhcp_wait_ms = 5000;
    if (!g_cfg.hostname[0])
        strcpy(g_cfg.hostname, "nspire");

    if (!g_lwip_ready) {
        lwip_init();
        g_lwip_ready = 1;
    }
    g_link = 0;
    g_server = 0;
    g_addr_logged = 0;
    netif_add(&g_netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL, netif_init_cb, ethernet_input);
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);

    uint8_t host_mac[6];
    memcpy(host_mac, g_cfg.mac, 6);
    host_mac[0] ^= 0x04;
    memcpy(g_host_mac, host_mac, 6);
    ncm_callbacks cb = {NULL, rx_frame, link_cb};
    if (ncm_open(host_mac, &cb) != 0) {
        netif_remove(&g_netif);
        nnet_dma_release();
        nnet_port_log("net: USB controller not available\n");
        return -1;
    }
    g_running = 1;
    nnet_port_log("net: started, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", g_cfg.mac[0], g_cfg.mac[1],
                  g_cfg.mac[2], g_cfg.mac[3], g_cfg.mac[4], g_cfg.mac[5]);
    return 0;
}

void nnet_stop(void)
{
    if (!g_running)
        return;
    dhcp_stop(&g_netif);
    dhcp_cleanup(&g_netif);
    dhcpd_stop();
    ncm_close();
    netif_set_link_down(&g_netif);
    netif_set_down(&g_netif);
    netif_remove(&g_netif);
    nnet_dma_release();
    g_running = 0;
    g_link = 0;
    g_server = 0;
    nnet_port_log("net: stopped\n");
}

int nnet_running(void) { return g_running; }

void nnet_poll(void)
{
    if (!g_running)
        return;
    ncm_poll();
    sys_check_timeouts();
    if (g_link && !g_server && g_pc_phase == 0 && g_cfg.mode == NNET_MODE_AUTO &&
        !dhcp_supplied_address(&g_netif) && (uint32_t)(nnet_port_ms() - g_link_ms) > g_cfg.dhcp_wait_ms) {
        dhcp_stop(&g_netif);
        become_server();
    }
    /* Serving the computer, but it has an address of its own elsewhere (a
     * fixed one, or it shares its connection and its DHCP server came up
     * late): ask its DHCP again, and if nothing answers, join its subnet. */
    if (g_link && g_server && g_pc_phase == 0 && g_cfg.mode == NNET_MODE_AUTO && g_pc_seen &&
        (g_pc_seen & g_server_mask) != (g_server_ip & g_server_mask)) {
        char a[16];
        nnet_format_ip(g_pc_seen, a, sizeof(a));
        nnet_port_log("net: the computer has %s of its own; asking its DHCP again\n", a);
        dhcpd_stop();
        g_server = 0;
        g_pc_peer = g_pc_seen;
        g_pc_phase = 1;
        g_pc_phase_ms = nnet_port_ms();
        g_addr_logged = 0;
        netif_set_addr(&g_netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
        dhcp_start(&g_netif);
    }
    if (g_link && g_pc_phase == 1 && !dhcp_supplied_address(&g_netif) &&
        (uint32_t)(nnet_port_ms() - g_pc_phase_ms) > 8000) {
        uint32_t pc = g_pc_peer, mask, ip;
        if ((pc >> 16) == 0xA9FE) {             /* link-local 169.254/16 */
            mask = 0xFFFF0000u;
            ip = 0xA9FE0000u | ((uint32_t)(1 + g_cfg.mac[4] % 254) << 8) | (uint32_t)(1 + g_cfg.mac[5] % 254);
        } else {
            mask = 0xFFFFFF00u;
            ip = (pc & mask) | (200u + g_cfg.mac[5] % 50u);
        }
        if (ip == pc)
            ip ^= 1;
        dhcp_stop(&g_netif);
        ip4_addr_t a, m, g;
        to_lwip(ip, &a);
        to_lwip(mask, &m);
        to_lwip(pc, &g);
        netif_set_addr(&g_netif, &a, &m, &g);
        ip_addr_t d;
        ip_addr_set_ip4_u32(&d, lwip_htonl(pc));
        dns_setserver(0, &d);
        g_pc_phase = 2;
        g_addr_logged = 0;
        char s1[16], s2[16];
        nnet_format_ip(ip, s1, sizeof(s1));
        nnet_format_ip(pc, s2, sizeof(s2));
        nnet_port_log("net: no DHCP answer; joining the computer's subnet as %s (computer %s)\n", s1, s2);
    }
    if ((uint32_t)(nnet_port_ms() - g_stats_ms) > 30000) {
        g_stats_ms = nnet_port_ms();
        const ncm_stats *n = ncm_get_stats();
        nnet_port_log("net: frames in %lu out %lu, ntbs in %lu (bad %lu), drops in %lu out %lu, state %d\n",
                      (unsigned long)g_rx_frames, (unsigned long)g_tx_frames, (unsigned long)n->rx_ntbs,
                      (unsigned long)n->rx_bad_ntbs, (unsigned long)g_rx_drops, (unsigned long)g_tx_drops,
                      nnet_state());
    }
    if (!g_addr_logged && nnet_state() == NNET_READY) {
        char a[16], b[16];
        g_addr_logged = 1;
        nnet_format_ip(nnet_local_ip(), a, sizeof(a));
        nnet_format_ip(nnet_peer_ip(), b, sizeof(b));
        nnet_port_log("net: ready, calculator %s, computer %s%s\n", a, b, g_server ? " (our DHCP)" : "");
    }
}

int nnet_state(void)
{
    if (!g_running)
        return NNET_OFF;
    if (!g_link)
        return NNET_WAIT_USB;
    if (ip4_addr_isany_val(*netif_ip4_addr(&g_netif)))
        return NNET_WAIT_ADDRESS;
    if (!g_server && g_pc_phase != 2 && (g_cfg.mode == NNET_MODE_AUTO || g_cfg.mode == NNET_MODE_CLIENT) &&
        !dhcp_supplied_address(&g_netif))
        return NNET_WAIT_ADDRESS;
    return NNET_READY;
}

uint32_t nnet_local_ip(void)
{
    if (!g_running)
        return 0;
    return from_lwip(netif_ip4_addr(&g_netif));
}

uint32_t nnet_peer_ip(void)
{
    if (nnet_state() != NNET_READY)
        return 0;
    if (g_server)
        return g_server_peer;
    if (g_pc_phase == 2)
        return g_pc_peer;
    if (g_cfg.mode == NNET_MODE_STATIC && g_cfg.peer)
        return g_cfg.peer;
    uint32_t gw = from_lwip(netif_ip4_gw(&g_netif));
    if (gw)
        return gw;
    struct dhcp *d = netif_dhcp_data(&g_netif);
    if (d)
        return lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(&d->server_ip_addr)));
    return 0;
}

int nnet_is_dhcp_server(void) { return g_server; }

void nnet_status_text(char *buf, int size)
{
    char a[16], b[16];
    switch (nnet_state()) {
    case NNET_OFF:
        snprintf(buf, (size_t)size, "USB network off");
        break;
    case NNET_WAIT_USB:
        snprintf(buf, (size_t)size, "USB: connect the cable to a computer");
        break;
    case NNET_WAIT_ADDRESS:
        snprintf(buf, (size_t)size, "USB: connected, getting an address...");
        break;
    default:
        nnet_format_ip(nnet_local_ip(), a, sizeof(a));
        nnet_format_ip(nnet_peer_ip(), b, sizeof(b));
        snprintf(buf, (size_t)size, "USB: %s, computer (\"pc\") %s", a, b);
        break;
    }
}

void nnet_stats_text(char *buf, int size)
{
    const ncm_stats *n = ncm_get_stats();
    snprintf(buf, (size_t)size, "ntb rx %lu (%lu frames, %lu bad) tx %lu frames, full %lu, drops rx %lu tx %lu",
             (unsigned long)n->rx_ntbs, (unsigned long)n->rx_frames, (unsigned long)n->rx_bad_ntbs,
             (unsigned long)n->tx_frames, (unsigned long)n->tx_full, (unsigned long)g_rx_drops,
             (unsigned long)g_tx_drops);
}

/* ------------------------------------------------------------------------ */
/* Names */

typedef struct lookup {
    char name[128];
    int state;              /* 0 free, 1 pending, 2 done, 3 failed */
    uint32_t ip;
    uint32_t when;
} lookup;
#define NNET_LOOKUPS 8
static lookup g_lookups[NNET_LOOKUPS];

static void dns_cb(const char *name, const ip_addr_t *addr, void *arg)
{
    (void)name;
    lookup *l = (lookup *)arg;
    if (l->state != 1)
        return;
    if (addr) {
        l->ip = lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(addr)));
        l->state = 2;
    } else {
        l->state = 3;
    }
}

static int name_is(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (x != y)
            return 0;
    }
    return *a == 0 && *b == 0;
}

int nnet_resolve(const char *name, uint32_t *ip)
{
    if (nnet_parse_ip(name, ip))
        return 1;
    if (name_is(name, "pc") || name_is(name, "computer") || name_is(name, "usb") || name_is(name, "host")) {
        *ip = nnet_peer_ip();
        return *ip ? 1 : (nnet_state() == NNET_READY ? -1 : 0);
    }
    if (strlen(name) >= sizeof(g_lookups[0].name) || nnet_state() != NNET_READY)
        return nnet_state() == NNET_READY ? -1 : 0;
    lookup *l = NULL;
    for (int i = 0; i < NNET_LOOKUPS; i++)
        if (g_lookups[i].state && !strcmp(g_lookups[i].name, name))
            l = &g_lookups[i];
    if (l) {
        if (l->state == 2) {
            *ip = l->ip;
            l->state = 0;
            return 1;
        }
        if (l->state == 3) {
            l->state = 0;
            return -1;
        }
        if ((uint32_t)(nnet_port_ms() - l->when) > 15000) {
            l->state = 0;
            return -1;
        }
        return 0;
    }
    for (int i = 0; i < NNET_LOOKUPS && !l; i++)
        if (!g_lookups[i].state)
            l = &g_lookups[i];
    if (!l) {
        l = &g_lookups[0];
    }
    strcpy(l->name, name);
    l->state = 1;
    l->when = nnet_port_ms();
    ip_addr_t addr;
    err_t e = dns_gethostbyname(name, &addr, dns_cb, l);
    if (e == ERR_OK) {
        *ip = lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(&addr)));
        l->state = 0;
        return 1;
    }
    if (e != ERR_INPROGRESS) {
        l->state = 0;
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* TCP */

struct nnet_tcp {
    struct tcp_pcb *pcb;
    int state;
    struct pbuf *rx;
};

static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)pcb;
    nnet_tcp *t = (nnet_tcp *)arg;
    if (t)
        t->state = err == ERR_OK ? NNET_TCP_OPEN : NNET_TCP_CLOSED;
    return ERR_OK;
}

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)pcb; (void)err;
    nnet_tcp *t = (nnet_tcp *)arg;
    if (!t) {
        if (p)
            pbuf_free(p);
        return ERR_OK;
    }
    if (!p) {
        t->state = NNET_TCP_CLOSED;     /* the server closed; what came stays readable */
        return ERR_OK;
    }
    if (t->rx)
        pbuf_cat(t->rx, p);
    else
        t->rx = p;
    return ERR_OK;
}

static void tcp_err_cb(void *arg, err_t err)
{
    (void)err;
    nnet_tcp *t = (nnet_tcp *)arg;
    if (t) {
        t->pcb = NULL;                  /* lwIP already freed it */
        t->state = NNET_TCP_CLOSED;
    }
}

nnet_tcp *nnet_tcp_connect(uint32_t ip, uint16_t port)
{
    if (nnet_state() != NNET_READY)
        return NULL;
    nnet_tcp *t = (nnet_tcp *)calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->pcb = tcp_new();
    if (!t->pcb) {
        free(t);
        return NULL;
    }
    tcp_arg(t->pcb, t);
    tcp_recv(t->pcb, tcp_recv_cb);
    tcp_err(t->pcb, tcp_err_cb);
    tcp_nagle_disable(t->pcb);
    ip_addr_t a;
    ip_addr_set_ip4_u32(&a, lwip_htonl(ip));
    t->state = NNET_TCP_CONNECTING;
    if (tcp_connect(t->pcb, &a, port, tcp_connected_cb) != ERR_OK) {
        tcp_abort(t->pcb);
        t->pcb = NULL;
        t->state = NNET_TCP_CLOSED;
    }
    return t;
}

int nnet_tcp_state(nnet_tcp *t) { return t ? t->state : NNET_TCP_CLOSED; }

int nnet_tcp_available(nnet_tcp *t) { return t && t->rx ? t->rx->tot_len : 0; }

int nnet_tcp_read(nnet_tcp *t, void *buf, int len)
{
    if (!t)
        return -1;
    if (!t->rx)
        return t->state == NNET_TCP_CLOSED ? -1 : 0;
    int n = t->rx->tot_len < len ? t->rx->tot_len : len;
    if (n > 0xFFFF)
        n = 0xFFFF;
    pbuf_copy_partial(t->rx, buf, (u16_t)n, 0);
    t->rx = pbuf_free_header(t->rx, (u16_t)n);
    if (t->pcb)
        tcp_recved(t->pcb, (u16_t)n);
    return n;
}

int nnet_tcp_write(nnet_tcp *t, const void *buf, int len)
{
    if (!t || !t->pcb || t->state != NNET_TCP_OPEN)
        return -1;
    int room = tcp_sndbuf(t->pcb);
    if (tcp_sndqueuelen(t->pcb) >= TCP_SND_QUEUELEN - 2)
        room = 0;
    int n = len < room ? len : room;
    if (n <= 0)
        return 0;
    if (tcp_write(t->pcb, buf, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK)
        return 0;
    tcp_output(t->pcb);
    return n;
}

void nnet_tcp_flush(nnet_tcp *t)
{
    if (t && t->pcb)
        tcp_output(t->pcb);
}

void nnet_tcp_close(nnet_tcp *t)
{
    if (!t)
        return;
    if (t->pcb) {
        tcp_arg(t->pcb, NULL);
        tcp_recv(t->pcb, NULL);
        tcp_err(t->pcb, NULL);
        if (tcp_close(t->pcb) != ERR_OK)
            tcp_abort(t->pcb);
        t->pcb = NULL;
    }
    if (t->rx)
        pbuf_free(t->rx);
    free(t);
}
