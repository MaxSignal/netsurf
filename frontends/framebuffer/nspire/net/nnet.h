/*
 * nnet.h -- the calculator's network over the USB cable: CDC NCM (ncm.c) to
 * the computer, lwIP on top, polled from the program's loop.
 *
 * Addressing (mode NNET_MODE_AUTO):
 *  - If the computer shares its connection (Windows "Internet Connection
 *    Sharing", NetworkManager "Shared to other computers", macOS Internet
 *    Sharing), it runs a DHCP server on the link and the calculator takes an
 *    address from it: servers on the computer, its LAN and the Internet are
 *    all reachable, names resolve through the DNS it hands out.
 *  - Otherwise the calculator becomes the DHCP server of the link after a few
 *    seconds: it takes 10.A.B.1/24 (A, B from its MAC address, so two
 *    calculators on one computer use different subnets) and gives the
 *    computer 10.A.B.2, without a gateway, so the computer's other connections
 *    stay as they are. Servers on the computer itself are reachable.
 * Either way "the computer" has an address: nnet_peer_ip().
 */
#ifndef NNET_H
#define NNET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NNET_MODE_AUTO = 0,     /* DHCP client, then DHCP server */
    NNET_MODE_CLIENT,       /* DHCP client only */
    NNET_MODE_SERVER,       /* DHCP server only */
    NNET_MODE_STATIC,       /* fixed address, no DHCP */
};

typedef struct nnet_config {
    uint8_t mac[6];         /* the calculator's; all zero: made up */
    int mode;
    uint32_t ip, netmask, gateway, dns;     /* host byte order; static mode,
                                             * or the server mode's subnet when
                                             * ip is not 0 */
    uint32_t peer;          /* static mode: the computer's address (0: gateway) */
    uint32_t dhcp_wait_ms;  /* auto: how long to wait for the computer's DHCP */
    char hostname[24];
} nnet_config;

void nnet_default_config(nnet_config *cfg);

/* Takes the USB controller from the OS and starts the stack. 0 on success. */
int nnet_start(const nnet_config *cfg);
/* Closes everything and gives the controller back to the OS. */
void nnet_stop(void);
int nnet_running(void);

/* USB events, received frames and lwIP's timers. Call often. */
void nnet_poll(void);

enum {
    NNET_OFF = 0,
    NNET_WAIT_USB,          /* no computer has opened the network yet */
    NNET_WAIT_ADDRESS,      /* link up, DHCP in progress */
    NNET_READY,
};
int nnet_state(void);
uint32_t nnet_local_ip(void);   /* host byte order, 0 if none */
uint32_t nnet_peer_ip(void);    /* the computer, 0 if not known */
int nnet_is_dhcp_server(void);
/* One line for the screen, e.g. "USB: 10.212.7.1, computer 10.212.7.2". */
void nnet_status_text(char *buf, int size);
void nnet_stats_text(char *buf, int size);

/* "a.b.c.d" (host byte order); 0 if it is not a dotted address. */
int nnet_parse_ip(const char *s, uint32_t *ip);
void nnet_format_ip(uint32_t ip, char *buf, int size);

/* Name lookup without blocking: 1 resolved (*ip set), 0 in progress (call
 * again), -1 failed. Dotted addresses and "pc"/"computer"/"usb" (the
 * computer at the other end of the cable) answer at once. */
int nnet_resolve(const char *name, uint32_t *ip);

/* TCP without blocking. */
typedef struct nnet_tcp nnet_tcp;
nnet_tcp *nnet_tcp_connect(uint32_t ip, uint16_t port);
enum { NNET_TCP_CONNECTING = 0, NNET_TCP_OPEN, NNET_TCP_CLOSED };
int nnet_tcp_state(nnet_tcp *t);
/* >0 bytes read, 0 nothing yet, -1 closed (and nothing left to read). */
int nnet_tcp_read(nnet_tcp *t, void *buf, int len);
int nnet_tcp_available(nnet_tcp *t);
/* Bytes accepted (0 when the send buffer is full), -1 closed. */
int nnet_tcp_write(nnet_tcp *t, const void *buf, int len);
void nnet_tcp_flush(nnet_tcp *t);
/* Closes the connection and frees t (the connection itself may linger to
 * deliver what was written). */
void nnet_tcp_close(nnet_tcp *t);

#ifdef __cplusplus
}
#endif

#endif
