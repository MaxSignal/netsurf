/* lwIP configuration for the TI-Nspire USB network: no OS (polled from the
 * program's loop), IPv4 only, TCP for the browser, UDP for DHCP and DNS.
 * Memory comes from the C heap as it is needed. */
#ifndef NNET_LWIPOPTS_H
#define NNET_LWIPOPTS_H

#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_TIMERS                 1
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0

#define MEM_LIBC_MALLOC             1
#define MEMP_MEM_MALLOC             1
#define MEM_ALIGNMENT               4

#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    0
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DHCP                   1
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define LWIP_AUTOIP                 0
#define LWIP_ACD                    0
#define LWIP_DNS                    1
#define DNS_MAX_SERVERS             2
#define DNS_TABLE_SIZE              8
#define DNS_MAX_NAME_LENGTH         128
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_BROADCAST_PING         0
#define IP_REASSEMBLY               1
#define IP_FRAG                     1

#define TCP_MSS                     1460
#define TCP_WND                     (16 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            32
#define MEMP_NUM_TCP_SEG            64
#define MEMP_NUM_TCP_PCB            16
#define MEMP_NUM_UDP_PCB            6
#define PBUF_POOL_SIZE              48
#define LWIP_TCP_KEEPALIVE          1
#define TCP_LISTEN_BACKLOG          0
#define LWIP_WND_SCALE              0

#define LWIP_STATS                  0
#define LWIP_STATS_DISPLAY          0

#include <stdint.h>
uint32_t nnet_rand(void);
#define LWIP_RAND()                 nnet_rand()

#endif
