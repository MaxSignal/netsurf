/*
 * nnet port on the calculator (Ndless, TI-Nspire CX).
 *
 * - Registers: the USB controller at 0xB0000000.
 * - DMA: TI-OS runs with the MMU on and the data cache write-back. Its page
 *   tables (OS 3.6: 1 MB sections, RAM 0x10000000-0x13FFFFFF mapped at the
 *   same addresses, cacheable and bufferable) are walked to turn a heap
 *   address into the bus address, falling back to the identity. Cache lines
 *   are cleaned / invalidated by address (CP15 c7).
 * - The OS's USB interrupt (VIC line 8, PL190 at 0xDC000000) is masked while
 *   the stack owns the controller.
 * - Time: the frontend's 32 kHz timer (sys_device.c).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nnet_port.h"
#include "../sys.h"

#define USB_BASE 0xB0000000u
#define USB_IRQ (1u << 8)

static volatile uint32_t *const vic_enable = (volatile uint32_t *)0xDC000010u;
static volatile uint32_t *const vic_enable_clear = (volatile uint32_t *)0xDC000014u;

#define MAX_BLOCKS 64
static void *blocks[MAX_BLOCKS];
static int nblocks;
static int irq_was_enabled;
static int irq_masked;
static int map_logged;

static uint32_t cp15_control(void)
{
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(v));
    return v;
}

static uint32_t cp15_ttb(void)
{
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(v));
    return v;
}

static int readable_table(uint32_t pa)
{
    /* where TI-OS keeps its tables (RAM, or the SRAM during boot), both
     * mapped at their own addresses */
    return (pa >= 0x10000000u && pa < 0x14000000u) || (pa >= 0xA4000000u && pa < 0xA4020000u);
}

static uint32_t translate(uint32_t va, int *ok)
{
    uint32_t ttb, l1, l2, t;

    *ok = 1;
    if (!(cp15_control() & 1u))
        return va;
    ttb = cp15_ttb() & ~0x3FFFu;
    if (!readable_table(ttb)) {
        *ok = 0;
        return va;
    }
    l1 = *(volatile uint32_t *)(ttb + (va >> 20) * 4u);
    switch (l1 & 3u) {
    case 2:
        return (l1 & 0xFFF00000u) | (va & 0x000FFFFFu);
    case 1:
        t = l1 & 0xFFFFFC00u;
        if (!readable_table(t)) { *ok = 0; return va; }
        l2 = *(volatile uint32_t *)(t + ((va >> 12) & 0xFFu) * 4u);
        break;
    case 3:
        t = l1 & 0xFFFFF000u;
        if (!readable_table(t)) { *ok = 0; return va; }
        l2 = *(volatile uint32_t *)(t + ((va >> 10) & 0x3FFu) * 4u);
        break;
    default:
        *ok = 0;
        return va;
    }
    switch (l2 & 3u) {
    case 1: return (l2 & 0xFFFF0000u) | (va & 0xFFFFu);
    case 2: return (l2 & 0xFFFFF000u) | (va & 0xFFFu);
    case 3: return (l2 & 0xFFFFFC00u) | (va & 0x3FFu);
    default:
        *ok = 0;
        return va;
    }
}

static void drain_write_buffer(void)
{
    uint32_t zero = 0;
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" : : "r"(zero) : "memory");
}

uint32_t nnet_port_reg_read(uint32_t offset)
{
    return *(volatile uint32_t *)(USB_BASE + offset);
}

void nnet_port_reg_write(uint32_t offset, uint32_t value)
{
    drain_write_buffer();   /* descriptors reach memory before the controller looks */
    *(volatile uint32_t *)(USB_BASE + offset) = value;
    drain_write_buffer();   /* and the write reaches the controller before the next read */
}

void *nnet_port_dma_alloc(size_t size, size_t align)
{
    void *raw;
    uintptr_t p;

    if (align < 32)
        align = 32;
    size = (size + 31u) & ~(size_t)31u;
    if (nblocks >= MAX_BLOCKS)
        return NULL;
    raw = malloc(size + align);
    if (raw == NULL)
        return NULL;
    blocks[nblocks++] = raw;
    p = ((uintptr_t)raw + align - 1u) & ~(uintptr_t)(align - 1u);
    memset((void *)p, 0, size);
    nnet_port_cache_clean((void *)p, size);
    return (void *)p;
}

void nnet_port_dma_free_all(void)
{
    int i;
    for (i = 0; i < nblocks; i++)
        free(blocks[i]);
    nblocks = 0;
}

uint32_t nnet_port_v2p(const void *p)
{
    int ok;
    uint32_t va = (uint32_t)(uintptr_t)p;
    uint32_t pa = translate(va, &ok);

    if (!map_logged) {
        map_logged = 1;
        nspire_log("usb: DMA %08lx -> bus %08lx (%s, mmu %s, ttb %08lx)\n",
                   (unsigned long)va, (unsigned long)pa, ok ? "page tables" : "identity",
                   (cp15_control() & 1u) ? "on" : "off", (unsigned long)cp15_ttb());
    }
    return pa;
}

void nnet_port_cache_clean(const void *p, size_t n)
{
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)31u;
    uintptr_t end = (uintptr_t)p + n;
    for (; a < end; a += 32u)
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" : : "r"(a) : "memory");
    drain_write_buffer();
}

void nnet_port_cache_inval(void *p, size_t n)
{
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)31u;
    uintptr_t end = (uintptr_t)p + n;
    for (; a < end; a += 32u)
        __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" : : "r"(a) : "memory");
}

uint32_t nnet_port_ms(void)
{
    return nspire_ms();
}

void nnet_port_udelay(uint32_t us)
{
    uint64_t start = nspire_us();
    while (nspire_us() - start < us) {
    }
}

void nnet_port_os_usb_irq(int masked)
{
    if (masked && !irq_masked) {
        irq_was_enabled = (*vic_enable & USB_IRQ) != 0;
        *vic_enable_clear = USB_IRQ;
        irq_masked = 1;
    } else if (!masked && irq_masked) {
        if (irq_was_enabled)
            *vic_enable = USB_IRQ;
        irq_masked = 0;
    }
}

uint32_t nnet_port_entropy(void)
{
    return nspire_entropy();
}

void nnet_port_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    nspire_vlog(fmt, ap);
    va_end(ap);
}
