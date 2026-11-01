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
 * TI-Nspire machine interface.
 *
 * What the TI-Nspire frontend needs from the machine it runs on;
 * sys_device.c implements it on the calculator, under Ndless.
 */

#ifndef NETSURF_FB_NSPIRE_SYS_H
#define NETSURF_FB_NSPIRE_SYS_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The LCD, landscape. */
#define NSPIRE_SCREEN_WIDTH 320
#define NSPIRE_SCREEN_HEIGHT 240

/** Keys the frontend reads. */
enum nspire_key {
	NK_ESC, NK_HOME, NK_MENU, NK_TAB, NK_CTRL, NK_SHIFT, NK_DEL, NK_ENTER,
	NK_RET, NK_CLICK, NK_UP, NK_DOWN, NK_LEFT, NK_RIGHT, NK_SPACE,
	NK_A, NK_B, NK_C, NK_D, NK_E, NK_F, NK_G, NK_H, NK_I, NK_J, NK_K,
	NK_L, NK_M, NK_N, NK_O, NK_P, NK_Q, NK_R, NK_S, NK_T, NK_U, NK_V,
	NK_W, NK_X, NK_Y, NK_Z,
	NK_0, NK_1, NK_2, NK_3, NK_4, NK_5, NK_6, NK_7, NK_8, NK_9,
	NK_PERIOD, NK_COMMA, NK_MINUS, NK_PLUS, NK_MULTIPLY, NK_DIVIDE,
	NK_EQU, NK_LP, NK_RP, NK_NEGATIVE, NK_QUESEXCL, NK_EXP, NK_EE,
	NK_PI, NK_FLAG, NK_SQU, NK_THETA, NK_QUOTE, NK_APOSTROPHE,
	NK_COLON, NK_LTHAN, NK_GTHAN, NK_BAR, NK_DOC, NK_SCRATCHPAD,
	NK_VAR, NK_CAT, NK_TRIG,
	NK_COUNT
};

/** Name of a key ("ESC", "A", ...), NULL past the end. */
const char *nspire_key_name(int key);

/** Start the clock, find the program's folder. */
void nspire_sys_init(int argc, char **argv);
/** Undo nspire_sys_init. */
void nspire_sys_fini(void);

/** Milliseconds since start (wraps after 49 days). */
uint32_t nspire_ms(void);
/** Microseconds since start. */
uint64_t nspire_us(void);
/** Seconds since 1970 from the calculator's clock. */
uint32_t nspire_unix_time(void);

/** The folder the program runs from, without the trailing slash. */
const char *nspire_app_dir(void);

/** Append a line to the log (netsurf_log.txt.tns next to the program). */
void nspire_log(const char *fmt, ...)
#ifdef __GNUC__
	__attribute__((format(printf, 1, 2)))
#endif
	;
void nspire_vlog(const char *fmt, va_list ap);

/** Prepare the LCD for 320x240 RGB565 frames. 0 on success. */
int nspire_screen_open(void);
void nspire_screen_close(void);
/** Show a whole frame. */
void nspire_screen_show(const uint16_t *frame);

/** Read the keypad and touchpad. */
void nspire_input_scan(void);
/** Was the key down at the last scan? */
bool nspire_key_down(int key);
/**
 * The touchpad at the last scan: true while touched, with the finger's
 * position in [0, *w) x [0, *h) (y grows downwards). False without one.
 */
bool nspire_touchpad(int *x, int *y, int *w, int *h);

/** Wait a little (up to ms) without spinning the CPU at full speed. */
void nspire_idle(int ms);

/** Bytes allocated through malloc and the most ever. */
size_t nspire_heap_used(void);
size_t nspire_heap_peak(void);

/** Hardware-ish noise for seeding random numbers. */
uint32_t nspire_entropy(void);

/** Show a message box (fatal errors). */
void nspire_message(const char *title, const char *text);

#ifdef __cplusplus
}
#endif

#endif
