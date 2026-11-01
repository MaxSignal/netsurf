/*
 * ci_regs.h -- the TI-Nspire CX's USB controller: a ChipIdea/ARC (TDI) dual
 * role core at 0xB0000000 (USB0, the mini-AB socket), the same core Linux
 * drives with ci_hdrc ("lsi,zevio-usb", operational registers 0x40 after the
 * capability registers at 0x100, forced to full speed).
 *
 * Device mode: endpoint queue heads (dQH, 64 bytes each, OUT then IN for every
 * endpoint, the list 2 KB aligned) and transfer descriptors (dTD, 32 bytes
 * aligned) in RAM, read and written by the controller.
 */
#ifndef CI_REGS_H
#define CI_REGS_H

#include <stdint.h>

#define CI_ID               0x000
#define CI_DCCPARAMS        0x124
#define CI_USBCMD           0x140
#define CI_USBSTS           0x144
#define CI_USBINTR          0x148
#define CI_FRINDEX          0x14C
#define CI_DEVICEADDR       0x154
#define CI_ENDPTLISTADDR    0x158
#define CI_BURSTSIZE        0x160
#define CI_PORTSC           0x184
#define CI_OTGSC            0x1A4
#define CI_USBMODE          0x1A8
#define CI_ENDPTSETUPSTAT   0x1AC
#define CI_ENDPTPRIME       0x1B0
#define CI_ENDPTFLUSH       0x1B4
#define CI_ENDPTSTAT        0x1B8
#define CI_ENDPTCOMPLETE    0x1BC
#define CI_ENDPTCTRL(n)     (0x1C0 + 4 * (n))

#define USBCMD_RS           (1u << 0)
#define USBCMD_RST          (1u << 1)
#define USBCMD_SUTW         (1u << 13)
#define USBCMD_ATDTW        (1u << 14)
#define USBCMD_ITC_MASK     (0xFFu << 16)

#define USBSTS_UI           (1u << 0)
#define USBSTS_UEI          (1u << 1)
#define USBSTS_PCI          (1u << 2)
#define USBSTS_URI          (1u << 6)
#define USBSTS_SRI          (1u << 7)
#define USBSTS_SLI          (1u << 8)

#define DEVICEADDR_USBADRA  (1u << 24)
#define DEVICEADDR_SHIFT    25

#define PORTSC_CCS          (1u << 0)
#define PORTSC_CSC          (1u << 1)
#define PORTSC_PEC          (1u << 3)
#define PORTSC_OCC          (1u << 5)
#define PORTSC_SUSP         (1u << 7)
#define PORTSC_PR           (1u << 8)
#define PORTSC_PHCD         (1u << 23)
#define PORTSC_PFSC         (1u << 24)
#define PORTSC_PSPD_SHIFT   26
#define PORTSC_W1C          (PORTSC_CSC | PORTSC_PEC | PORTSC_OCC)

#define OTGSC_BSV           (1u << 11)

#define USBMODE_CM_MASK     0x3u
#define USBMODE_CM_DEVICE   0x2u
#define USBMODE_SLOM        (1u << 3)

#define EPCTRL_RXS          (1u << 0)
#define EPCTRL_RXT_SHIFT    2
#define EPCTRL_RXR          (1u << 6)
#define EPCTRL_RXE          (1u << 7)
#define EPCTRL_TXS          (1u << 16)
#define EPCTRL_TXT_SHIFT    18
#define EPCTRL_TXR          (1u << 22)
#define EPCTRL_TXE          (1u << 23)
#define EPTYPE_CONTROL      0u
#define EPTYPE_ISO          1u
#define EPTYPE_BULK         2u
#define EPTYPE_INT          3u

/* Bit of an endpoint in ENDPTPRIME/FLUSH/STAT/COMPLETE. */
#define EPBIT(num, in)      ((in) ? (0x10000u << (num)) : (1u << (num)))

#define TD_TERMINATE        1u
#define TD_STATUS_MASK      0xFFu
#define TD_ACTIVE           (1u << 7)
#define TD_HALTED           (1u << 6)
#define TD_DBERR            (1u << 5)
#define TD_TRERR            (1u << 3)
#define TD_IOC              (1u << 15)
#define TD_BYTES_SHIFT      16
#define TD_BYTES_MASK       0x7FFFu

#define QH_IOS              (1u << 15)
#define QH_MAXPKT_SHIFT     16
#define QH_ZLT              (1u << 29)

typedef struct ci_dtd {
    volatile uint32_t next;
    volatile uint32_t token;
    volatile uint32_t buf[5];
    uint32_t pad;               /* one dTD per 32-byte cache line */
} ci_dtd;

typedef struct ci_dqh {
    volatile uint32_t cap;
    volatile uint32_t current;
    volatile uint32_t next;     /* overlay: next dTD */
    volatile uint32_t token;    /* overlay: token */
    volatile uint32_t buf[5];
    volatile uint32_t reserved;
    volatile uint32_t setup[2];
    uint32_t pad[4];
} ci_dqh;

#endif
