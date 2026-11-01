/*
 * Copyright 2026 The NetSurf Browser Project
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * The TI-Nspire under Ndless.
 *
 * Clock: Ndless' newlib answers gettimeofday() from the RTC, which counts
 * whole seconds. The first SP804 timer (0x900C0000) runs free here at
 * 32768 Hz -- set up the way nSDL does on the CX and CX II, and restored on
 * exit -- and backs nspire_us() and, through the linker's
 * --wrap=_gettimeofday, newlib's clock, with the RTC as the base for the
 * date.
 *
 * Heap: the OS allocator promises 4-byte alignment; every block is handed
 * out 8-byte aligned (--wrap=malloc,...), which 64-bit values need, and the
 * wrapper counts what is in use.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>

#include <libndls.h>
#include <os.h>
#undef sleep

#include "framebuffer/nspire/sys.h"

static volatile uint32_t *const timer_load = (volatile uint32_t *)0x900C0000;
static volatile uint32_t *const timer_value = (volatile uint32_t *)0x900C0004;
static volatile uint32_t *const timer_control = (volatile uint32_t *)0x900C0008;
static volatile uint32_t *const timer_clock = (volatile uint32_t *)0x900C0080;
static volatile uint32_t *const clock_gates = (volatile uint32_t *)0x900B0018;
static volatile uint32_t *const rtc_seconds = (volatile uint32_t *)0x90090000;
#define TIMER_GATE_BIT (1u << 11)
#define TIMER_HZ 32768u
/* A larger step between two reads means the counter was touched behind our
 * back; it is dropped rather than turned into a jump of hours. */
#define MAX_STEP_TICKS (TIMER_HZ * 30u)
/* The RTC counts from 1997-01-01. */
#define RTC_EPOCH 852076800u

static uint32_t saved_control, saved_load, saved_clock, saved_gates;
static uint32_t last_value;
static uint64_t ticks;
static uint32_t rtc_base;
static int timer_running;
static char app_dir[256] = "/documents/ndless";
static int log_started;
static int lcd_ready;

static void log_task_stack(const char *when);

static void timer_start(void)
{
	saved_gates = *clock_gates;
	saved_control = *timer_control;
	saved_load = *timer_load;
	saved_clock = *timer_clock;
	*clock_gates = saved_gates & ~TIMER_GATE_BIT;
	*timer_control = 0;
	*timer_clock = 0xA;		/* 32768 Hz source */
	*timer_load = 0xFFFFFFFFu;
	*timer_control = 0x82u;		/* enable, free running, 32-bit */
	last_value = *timer_value;
	ticks = 0;
	rtc_base = *rtc_seconds;
	timer_running = 1;
}

static void timer_stop(void)
{
	if (!timer_running)
		return;
	*timer_control = 0;
	*timer_load = saved_load;
	*timer_clock = saved_clock;
	*timer_control = saved_control;
	*clock_gates = saved_gates;
	timer_running = 0;
}

static uint64_t timer_ticks(void)
{
	uint32_t value, step;

	if (!timer_running)
		return 0;
	value = *timer_value;
	step = last_value - value;	/* down counter */
	if (step <= MAX_STEP_TICKS)
		ticks += step;
	last_value = value;
	return ticks;
}

uint64_t nspire_us(void)
{
	/* ticks * 1e6 / 32768 == ticks * 15625 / 512 */
	return timer_ticks() * 15625u / 512u;
}

uint32_t nspire_ms(void)
{
	return (uint32_t)(timer_ticks() * 1000u / TIMER_HZ);
}

uint32_t nspire_unix_time(void)
{
	return RTC_EPOCH + rtc_base + (uint32_t)(nspire_us() / 1000000u);
}

/* newlib's clock (time(), gettimeofday()) */
int __wrap__gettimeofday(struct timeval *tv, void *tz);
int __wrap__gettimeofday(struct timeval *tv, void *tz)
{
	if (tv != NULL) {
		uint64_t us = nspire_us();
		tv->tv_sec = (time_t)(RTC_EPOCH + rtc_base + us / 1000000u);
		tv->tv_usec = (suseconds_t)(us % 1000000u);
	}
	if (tz != NULL)
		memset(tz, 0, sizeof(struct timezone));
	return 0;
}

void nspire_sys_init(int argc, char **argv)
{
	timer_start();
	if (argc > 0 && argv != NULL && argv[0] != NULL) {
		const char *slash = strrchr(argv[0], '/');
		if (slash != NULL && (size_t)(slash - argv[0]) < sizeof(app_dir)) {
			memcpy(app_dir, argv[0], (size_t)(slash - argv[0]));
			app_dir[slash - argv[0]] = '\0';
		}
	}
	log_task_stack("start");
}

/* The program's TI-OS task (Nucleus PLUS): its control block records the
 * stack (OS 3.6: id 'TASK' at word 3, start / end / pointer / size /
 * minimum unused at words 9-13). Logged at start and exit: how much stack
 * the program has, and how much of it was ever used. */
static void log_task_stack(const char *when)
{
	volatile uint32_t *t = TCC_Current_Task_Pointer();
	uintptr_t a = (uintptr_t)t;
	uint32_t sp;

	__asm__ volatile("mov %0, sp" : "=r"(sp));
	if (t == NULL || (a & 3u) || a < 0x10000000u || a >= 0x14000000u ||
	    t[3] != 0x5441534Bu) {
		nspire_log("task (%s): no control block, sp %08lx\n", when, (unsigned long)sp);
		return;
	}
	nspire_log("task (%s): stack %08lx-%08lx, %lu bytes, %lu never used, sp %08lx\n",
		   when, (unsigned long)t[9], (unsigned long)t[10], (unsigned long)t[12],
		   (unsigned long)t[13], (unsigned long)sp);
}

void nspire_sys_fini(void)
{
	log_task_stack("exit");
	nspire_screen_close();
	timer_stop();
}

const char *nspire_app_dir(void)
{
	return app_dir;
}

void nspire_vlog(const char *fmt, va_list ap)
{
	char path[300];
	FILE *f;

	/* The OS commits a file to flash only when it is closed: open and
	 * close per line, so a hang keeps the lines before it. */
	snprintf(path, sizeof(path), "%s/netsurf_log.txt.tns", app_dir);
	f = fopen(path, log_started ? "a" : "w");
	if (f == NULL)
		return;
	log_started = 1;
	fprintf(f, "%7lu ", (unsigned long)nspire_ms());
	vfprintf(f, fmt, ap);
	fclose(f);
}

void nspire_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	nspire_vlog(fmt, ap);
	va_end(ap);
}

/* ------------------------------------------------------------------------ */
/* Screen */

/* The CX II (and CX HW-W) panel is 240x320 portrait; lcd_blit rotates. */
int nspire_screen_open(void)
{
	if (!lcd_init(SCR_320x240_565))
		return -1;
	lcd_ready = 1;
	return 0;
}

void nspire_screen_close(void)
{
	if (lcd_ready) {
		lcd_init(SCR_TYPE_INVALID);
		lcd_ready = 0;
	}
}

void nspire_screen_show(const uint16_t *frame)
{
	lcd_blit((void *)frame, SCR_320x240_565);
}

/* ------------------------------------------------------------------------ */
/* Keypad and touchpad */

static const t_key *key_table(int key)
{
	static const t_key table[NK_COUNT] = {
		KEY_NSPIRE_ESC, KEY_NSPIRE_HOME, KEY_NSPIRE_MENU, KEY_NSPIRE_TAB,
		KEY_NSPIRE_CTRL, KEY_NSPIRE_SHIFT, KEY_NSPIRE_DEL, KEY_NSPIRE_ENTER,
		KEY_NSPIRE_RET, KEY_NSPIRE_CLICK, KEY_NSPIRE_UP, KEY_NSPIRE_DOWN,
		KEY_NSPIRE_LEFT, KEY_NSPIRE_RIGHT, KEY_NSPIRE_SPACE,
		KEY_NSPIRE_A, KEY_NSPIRE_B, KEY_NSPIRE_C, KEY_NSPIRE_D, KEY_NSPIRE_E,
		KEY_NSPIRE_F, KEY_NSPIRE_G, KEY_NSPIRE_H, KEY_NSPIRE_I, KEY_NSPIRE_J,
		KEY_NSPIRE_K, KEY_NSPIRE_L, KEY_NSPIRE_M, KEY_NSPIRE_N, KEY_NSPIRE_O,
		KEY_NSPIRE_P, KEY_NSPIRE_Q, KEY_NSPIRE_R, KEY_NSPIRE_S, KEY_NSPIRE_T,
		KEY_NSPIRE_U, KEY_NSPIRE_V, KEY_NSPIRE_W, KEY_NSPIRE_X, KEY_NSPIRE_Y,
		KEY_NSPIRE_Z,
		KEY_NSPIRE_0, KEY_NSPIRE_1, KEY_NSPIRE_2, KEY_NSPIRE_3, KEY_NSPIRE_4,
		KEY_NSPIRE_5, KEY_NSPIRE_6, KEY_NSPIRE_7, KEY_NSPIRE_8, KEY_NSPIRE_9,
		KEY_NSPIRE_PERIOD, KEY_NSPIRE_COMMA, KEY_NSPIRE_MINUS, KEY_NSPIRE_PLUS,
		KEY_NSPIRE_MULTIPLY, KEY_NSPIRE_DIVIDE, KEY_NSPIRE_EQU, KEY_NSPIRE_LP,
		KEY_NSPIRE_RP, KEY_NSPIRE_NEGATIVE, KEY_NSPIRE_QUESEXCL, KEY_NSPIRE_EXP,
		KEY_NSPIRE_EE, KEY_NSPIRE_PI, KEY_NSPIRE_FLAG, KEY_NSPIRE_SQU,
		KEY_NSPIRE_THETA, KEY_NSPIRE_QUOTE, KEY_NSPIRE_APOSTROPHE,
		KEY_NSPIRE_COLON, KEY_NSPIRE_LTHAN, KEY_NSPIRE_GTHAN, KEY_NSPIRE_BAR,
		KEY_NSPIRE_DOC, KEY_NSPIRE_SCRATCHPAD, KEY_NSPIRE_VAR, KEY_NSPIRE_CAT,
		KEY_NSPIRE_TRIG,
	};
	return &table[key];
}

static uint8_t key_state[NK_COUNT];
static int pad_touched, pad_x, pad_y, pad_w, pad_h;

void nspire_input_scan(void)
{
	int k;

	for (k = 0; k < NK_COUNT; k++)
		key_state[k] = isKeyPressed(*key_table(k)) ? 1 : 0;

	pad_touched = 0;
	if (is_touchpad) {
		touchpad_info_t *info = touchpad_getinfo();
		touchpad_report_t report;
		if (info != NULL && touchpad_scan(&report) == 0 && report.contact) {
			pad_touched = 1;
			pad_w = info->width;
			pad_h = info->height;
			pad_x = report.x;
			pad_y = info->height - 1 - report.y;	/* y counts up */
		}
	}
}

bool nspire_key_down(int key)
{
	return key >= 0 && key < NK_COUNT && key_state[key];
}

bool nspire_touchpad(int *x, int *y, int *w, int *h)
{
	if (!pad_touched)
		return false;
	*x = pad_x;
	*y = pad_y;
	*w = pad_w;
	*h = pad_h;
	return true;
}

void nspire_idle(int ms)
{
	if (ms > 0)
		msleep((unsigned)ms);
}

uint32_t nspire_entropy(void)
{
	uint32_t x = *timer_value * 2654435761u;
	x ^= *rtc_seconds * 40503u;
	x ^= (uint32_t)(uintptr_t)&x;
	x ^= (uint32_t)nspire_heap_used() * 69069u;
	return x;
}

void nspire_message(const char *title, const char *text)
{
	int had_lcd = lcd_ready;

	/* the OS dialog draws on the OS's screen */
	nspire_screen_close();
	show_msgbox(title, text);
	if (had_lcd)
		nspire_screen_open();
}

/* ------------------------------------------------------------------------ */
/* Heap: 8-byte aligned blocks with a header (size, magic) */

void *__real_malloc(size_t size);
void __real_free(void *p);
void *__real_realloc(void *p, size_t size);
void *__wrap_malloc(size_t size);
void __wrap_free(void *p);
void *__wrap_realloc(void *p, size_t size);
void *__wrap_calloc(size_t count, size_t size);

#define HEAP_MAGIC 0x4E535048u	/* "NSPH" */
#define HEAP_ALIGN 8u

struct heap_header {
	uint32_t offset;	/* from the OS block to the user pointer */
	uint32_t size;
	uint32_t magic;		/* last, right below the user pointer */
};

static size_t heap_used, heap_peak;

static struct heap_header *header_of(void *p)
{
	struct heap_header *h;

	if (((uintptr_t)p & (HEAP_ALIGN - 1)) != 0)
		return NULL;
	h = (struct heap_header *)((unsigned char *)p - sizeof(*h));
	return h->magic == HEAP_MAGIC ? h : NULL;
}

void *__wrap_malloc(size_t size)
{
	unsigned char *raw;
	uintptr_t user;
	struct heap_header *h;

	if (size > (size_t)-1 - sizeof(*h) - HEAP_ALIGN)
		return NULL;
	raw = __real_malloc(size + sizeof(*h) + HEAP_ALIGN - 1);
	if (raw == NULL)
		return NULL;
	user = ((uintptr_t)raw + sizeof(*h) + HEAP_ALIGN - 1) & ~(uintptr_t)(HEAP_ALIGN - 1);
	h = (struct heap_header *)(user - sizeof(*h));
	h->offset = (uint32_t)(user - (uintptr_t)raw);
	h->size = (uint32_t)size;
	h->magic = HEAP_MAGIC;
	heap_used += size;
	if (heap_used > heap_peak)
		heap_peak = heap_used;
	return (void *)user;
}

void __wrap_free(void *p)
{
	struct heap_header *h;

	if (p == NULL)
		return;
	h = header_of(p);
	if (h == NULL) {
		/* allocated inside newlib by the unwrapped malloc */
		__real_free(p);
		return;
	}
	heap_used -= h->size;
	h->magic = 0;
	__real_free((unsigned char *)p - h->offset);
}

void *__wrap_realloc(void *p, size_t size)
{
	struct heap_header *h;
	void *n;

	if (p == NULL)
		return __wrap_malloc(size);
	if (size == 0) {
		__wrap_free(p);
		return NULL;
	}
	h = header_of(p);
	if (h == NULL)
		return __real_realloc(p, size);
	if (size <= h->size && size + 256 > h->size) {
		heap_used -= h->size - size;
		h->size = (uint32_t)size;
		return p;
	}
	n = __wrap_malloc(size);
	if (n == NULL)
		return NULL;
	memcpy(n, p, h->size < size ? h->size : size);
	__wrap_free(p);
	return n;
}

void *__wrap_calloc(size_t count, size_t size)
{
	void *p;

	if (size != 0 && count > (size_t)-1 / size)
		return NULL;
	p = __wrap_malloc(count * size);
	if (p != NULL)
		memset(p, 0, count * size);
	return p;
}

size_t nspire_heap_used(void)
{
	return heap_used;
}

size_t nspire_heap_peak(void)
{
	return heap_peak;
}

/* ------------------------------------------------------------------------ */
/* POSIX bits newlib under Ndless lacks */

int usleep(useconds_t usec);
int usleep(useconds_t usec)
{
	msleep((unsigned)((usec + 999u) / 1000u));
	return 0;
}

unsigned sleep(unsigned seconds);
unsigned sleep(unsigned seconds)
{
	msleep(seconds * 1000u);
	return 0;
}
