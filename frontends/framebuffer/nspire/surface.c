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
 * The machine side of libnsfb's "nspire" surface: the frame goes to the
 * LCD, the keypad and touchpad become pointer and key events.
 *
 * Keypad:
 *   touchpad (slide)        move the pointer
 *   touchpad click, enter   click (enter is Return while typing)
 *   ctrl + slide            scroll
 *   arrows                  scroll the page / move the caret
 *   ctrl + up/down          page up / page down
 *   letters, digits, ...    type; shift for capitals and the second sign
 *   del                     backspace (shift + del: delete)
 *   ret                     Return
 *   tab                     next field
 *   esc                     back (while typing: leave the field)
 *   doc                     forward
 *   menu                    the address bar
 *   scratchpad              reload
 *   var                     home page
 *   ctrl + / ctrl -         zoom (ctrl 0: 100 %)
 *   home (on)               quit
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <libnsfb.h>
#include <libnsfb_event.h>
#include <libnsfb_nspire.h>

#include "framebuffer/nspire/nspire.h"
#include "framebuffer/nspire/sys.h"

static const char *const key_names[NK_COUNT] = {
	"ESC", "HOME", "MENU", "TAB", "CTRL", "SHIFT", "DEL", "ENTER",
	"RET", "CLICK", "UP", "DOWN", "LEFT", "RIGHT", "SPACE",
	"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K",
	"L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V",
	"W", "X", "Y", "Z",
	"0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
	"PERIOD", "COMMA", "MINUS", "PLUS", "MULTIPLY", "DIVIDE",
	"EQU", "LP", "RP", "NEGATIVE", "QUESEXCL", "EXP", "EE",
	"PI", "FLAG", "SQU", "THETA", "QUOTE", "APOSTROPHE",
	"COLON", "LTHAN", "GTHAN", "BAR", "DOC", "SCRATCHPAD",
	"VAR", "CAT", "TRIG",
};

const char *nspire_key_name(int key)
{
	if (key < 0 || key >= NK_COUNT)
		return NULL;
	return key_names[key];
}

/* Signs on keys other than letters: normal and with shift. */
static const struct {
	int key;
	char plain;
	char shifted;
} sign_keys[] = {
	{ NK_SPACE, ' ', ' ' },
	{ NK_PERIOD, '.', ':' },
	{ NK_COMMA, ',', ';' },
	{ NK_MINUS, '-', '_' },
	{ NK_NEGATIVE, '-', '~' },
	{ NK_PLUS, '+', '@' },
	{ NK_MULTIPLY, '*', '&' },
	{ NK_DIVIDE, '/', '\\' },
	{ NK_EQU, '=', '#' },
	{ NK_LP, '(', '[' },
	{ NK_RP, ')', ']' },
	{ NK_QUESEXCL, '?', '!' },
	{ NK_EXP, '^', '%' },
	{ NK_EE, '@', '$' },
	{ NK_PI, '$', '$' },
	{ NK_FLAG, '|', '|' },
	{ NK_SQU, '%', '%' },
	{ NK_THETA, '~', '~' },
	{ NK_QUOTE, '"', '"' },
	{ NK_APOSTROPHE, '\'', '`' },
	{ NK_COLON, ':', ';' },
	{ NK_LTHAN, '<', '{' },
	{ NK_GTHAN, '>', '}' },
	{ NK_BAR, '|', '|' },
};

#define QUEUE_SIZE 64
static nsfb_event_t queue[QUEUE_SIZE];
static int queue_head, queue_tail;

static int screen_w = NSPIRE_SCREEN_WIDTH;
static int screen_h = NSPIRE_SCREEN_HEIGHT;
static const uint16_t *frame_ptr;
static bool frame_dirty;

/* pointer, in 1/256 pixel */
static int ptr_x = (NSPIRE_SCREEN_WIDTH / 2) << 8;
static int ptr_y = (NSPIRE_SCREEN_HEIGHT / 2) << 8;
static bool pad_was_touched;
static int pad_last_x, pad_last_y;
static int scroll_acc;

static uint8_t prev_keys[NK_COUNT];
static uint32_t repeat_at[NK_COUNT];
static bool mouse_down;		/* button 1 held through click or enter */
static int mouse_key = -1;	/* the key holding it */

#define REPEAT_DELAY 450
#define REPEAT_RATE 70

static void push(const nsfb_event_t *ev)
{
	int next = (queue_tail + 1) % QUEUE_SIZE;
	if (next == queue_head)
		return;		/* full: drop */
	queue[queue_tail] = *ev;
	queue_tail = next;
}

static void push_key(enum nsfb_event_type_e type, enum nsfb_key_code_e code)
{
	nsfb_event_t ev;
	ev.type = type;
	ev.value.keycode = code;
	push(&ev);
}

static void push_tap(enum nsfb_key_code_e code)
{
	push_key(NSFB_EVENT_KEY_DOWN, code);
	push_key(NSFB_EVENT_KEY_UP, code);
}

static void push_move(void)
{
	nsfb_event_t ev;
	ev.type = NSFB_EVENT_MOVE_ABSOLUTE;
	ev.value.vector.x = ptr_x >> 8;
	ev.value.vector.y = ptr_y >> 8;
	ev.value.vector.z = 0;
	push(&ev);
}

static void push_with(enum nsfb_key_code_e modifier, enum nsfb_key_code_e code)
{
	push_key(NSFB_EVENT_KEY_DOWN, modifier);
	push_tap(code);
	push_key(NSFB_EVENT_KEY_UP, modifier);
}

/* A character as fbtk's US keymap produces it. */
static void push_char(char c)
{
	switch (c) {
	case '%': push_with(NSFB_KEY_LSHIFT, NSFB_KEY_5); return;
	case '^': push_with(NSFB_KEY_LSHIFT, NSFB_KEY_6); return;
	case '{': push_with(NSFB_KEY_LSHIFT, NSFB_KEY_LEFTBRACKET); return;
	case '}': push_with(NSFB_KEY_LSHIFT, NSFB_KEY_RIGHTBRACKET); return;
	case '|': push_with(NSFB_KEY_LSHIFT, NSFB_KEY_BACKSLASH); return;
	case '~': push_tap((enum nsfb_key_code_e)94); return;
	default: break;
	}
	if (c >= 'A' && c <= 'Z') {
		push_with(NSFB_KEY_LSHIFT, (enum nsfb_key_code_e)(c - 'A' + 'a'));
		return;
	}
	push_tap((enum nsfb_key_code_e)(unsigned char)c);
}

static void move_pointer(int dx, int dy)
{
	ptr_x += dx;
	ptr_y += dy;
	if (ptr_x < 0) ptr_x = 0;
	if (ptr_y < 0) ptr_y = 0;
	if (ptr_x > ((screen_w - 1) << 8)) ptr_x = (screen_w - 1) << 8;
	if (ptr_y > ((screen_h - 1) << 8)) ptr_y = (screen_h - 1) << 8;
	push_move();
}

static void touchpad(bool ctrl)
{
	int x, y, w, h;

	if (!nspire_touchpad(&x, &y, &w, &h) || w <= 0 || h <= 0) {
		pad_was_touched = false;
		scroll_acc = 0;
		return;
	}
	if (pad_was_touched) {
		/* the pad's width moves the pointer across 1.5 screens */
		int dx = (x - pad_last_x) * (screen_w * 384) / w;
		int dy = (y - pad_last_y) * (screen_h * 384) / h;
		if (ctrl) {
			scroll_acc += dy;
			while (scroll_acc <= -(16 << 8)) {
				push_tap(NSFB_KEY_MOUSE_4);
				scroll_acc += 16 << 8;
			}
			while (scroll_acc >= (16 << 8)) {
				push_tap(NSFB_KEY_MOUSE_5);
				scroll_acc -= 16 << 8;
			}
		} else if (dx != 0 || dy != 0) {
			move_pointer(dx, dy);
		}
	}
	pad_was_touched = true;
	pad_last_x = x;
	pad_last_y = y;
}

/* true the moment a key goes down, and again while it is held */
static bool pressed(int k, uint32_t now, bool repeats)
{
	if (!nspire_key_down(k))
		return false;
	if (!prev_keys[k]) {
		repeat_at[k] = now + REPEAT_DELAY;
		return true;
	}
	if (repeats && (int32_t)(now - repeat_at[k]) >= 0) {
		repeat_at[k] = now + REPEAT_RATE;
		return true;
	}
	return false;
}

static void scan(void)
{
	uint32_t now = nspire_ms();
	bool shift, ctrl, typing;
	int k;
	size_t i;

	nspire_input_scan();
	shift = nspire_key_down(NK_SHIFT);
	ctrl = nspire_key_down(NK_CTRL);
	typing = fb_nspire_text_input_active();

	touchpad(ctrl);

	/* button 1: the touchpad's click, or enter when not typing */
	if (!mouse_down) {
		if (nspire_key_down(NK_CLICK) && !prev_keys[NK_CLICK])
			mouse_key = NK_CLICK;
		else if (!typing && nspire_key_down(NK_ENTER) && !prev_keys[NK_ENTER])
			mouse_key = NK_ENTER;
		if (mouse_key >= 0) {
			push_move();
			push_key(NSFB_EVENT_KEY_DOWN, NSFB_KEY_MOUSE_1);
			mouse_down = true;
		}
	} else if (!nspire_key_down(mouse_key)) {
		push_key(NSFB_EVENT_KEY_UP, NSFB_KEY_MOUSE_1);
		mouse_down = false;
		mouse_key = -1;
	}

	if (pressed(NK_HOME, now, false)) {
		nsfb_event_t ev;
		ev.type = NSFB_EVENT_CONTROL;
		ev.value.controlcode = NSFB_CONTROL_QUIT;
		push(&ev);
	}

	if (pressed(NK_ENTER, now, true) && typing && mouse_key != NK_ENTER)
		push_tap(NSFB_KEY_RETURN);
	if (pressed(NK_RET, now, true))
		push_tap(NSFB_KEY_RETURN);
	if (pressed(NK_TAB, now, true))
		push_tap(NSFB_KEY_TAB);

	if (pressed(NK_DEL, now, true)) {
		if (shift || ctrl)
			push_tap(NSFB_KEY_DELETE);
		else
			push_tap(NSFB_KEY_BACKSPACE);
	}

	if (pressed(NK_UP, now, true))
		push_tap(ctrl ? NSFB_KEY_PAGEUP : NSFB_KEY_UP);
	if (pressed(NK_DOWN, now, true))
		push_tap(ctrl ? NSFB_KEY_PAGEDOWN : NSFB_KEY_DOWN);
	if (pressed(NK_LEFT, now, true)) {
		if (shift)
			push_with(NSFB_KEY_LSHIFT, NSFB_KEY_LEFT);
		else
			push_tap(NSFB_KEY_LEFT);
	}
	if (pressed(NK_RIGHT, now, true)) {
		if (shift)
			push_with(NSFB_KEY_LSHIFT, NSFB_KEY_RIGHT);
		else
			push_tap(NSFB_KEY_RIGHT);
	}

	if (pressed(NK_ESC, now, false))
		fb_nspire_action(typing ? NSPIRE_ACTION_UNFOCUS : NSPIRE_ACTION_BACK);
	if (pressed(NK_DOC, now, false))
		fb_nspire_action(NSPIRE_ACTION_FORWARD);
	if (pressed(NK_MENU, now, false))
		fb_nspire_action(NSPIRE_ACTION_URLBAR);
	if (pressed(NK_SCRATCHPAD, now, false))
		fb_nspire_action(NSPIRE_ACTION_RELOAD);
	if (pressed(NK_VAR, now, false))
		fb_nspire_action(NSPIRE_ACTION_HOME);

	/* letters */
	for (k = NK_A; k <= NK_Z; k++) {
		if (!pressed(k, now, true))
			continue;
		if (ctrl)
			push_with(NSFB_KEY_LCTRL, (enum nsfb_key_code_e)('a' + k - NK_A));
		else
			push_char((char)((shift ? 'A' : 'a') + k - NK_A));
	}

	/* digits; ctrl 0 resets the zoom */
	for (k = NK_0; k <= NK_9; k++) {
		if (!pressed(k, now, true))
			continue;
		if (ctrl)
			push_with(NSFB_KEY_LCTRL, (enum nsfb_key_code_e)('0' + k - NK_0));
		else
			push_char((char)('0' + k - NK_0));
	}

	for (i = 0; i < sizeof(sign_keys) / sizeof(sign_keys[0]); i++) {
		k = sign_keys[i].key;
		if (!pressed(k, now, true))
			continue;
		if (ctrl && k == NK_PLUS)
			push_with(NSFB_KEY_LCTRL, NSFB_KEY_EQUALS);
		else if (ctrl && (k == NK_MINUS || k == NK_NEGATIVE))
			push_with(NSFB_KEY_LCTRL, NSFB_KEY_MINUS);
		else
			push_char(shift ? sign_keys[i].shifted : sign_keys[i].plain);
	}

	for (k = 0; k < NK_COUNT; k++)
		prev_keys[k] = nspire_key_down(k) ? 1 : 0;
}

static void show_frame(void)
{
	if (frame_dirty && frame_ptr != NULL) {
		nspire_screen_show(frame_ptr);
		frame_dirty = false;
	}
}

/* ------------------------------------------------------------------------ */
/* libnsfb_nspire.h */

int nsfb_nspire_open(int *width, int *height)
{
	if (nspire_screen_open() != 0)
		return -1;
	*width = screen_w;
	*height = screen_h;
	return 0;
}

void nsfb_nspire_close(void)
{
	nspire_screen_close();
	frame_ptr = NULL;
}

void nsfb_nspire_present(const uint16_t *frame, int stride, const nsfb_bbox_t *box)
{
	(void)stride;
	(void)box;
	/* shown once per pass through the main loop, when it next waits */
	frame_ptr = frame;
	frame_dirty = true;
}

bool nsfb_nspire_input(nsfb_event_t *event, int timeout)
{
	uint32_t start = nspire_ms();

	show_frame();

	for (;;) {
		if (queue_head == queue_tail)
			scan();
		if (queue_head != queue_tail) {
			*event = queue[queue_head];
			queue_head = (queue_head + 1) % QUEUE_SIZE;
			return true;
		}
		if (timeout >= 0 && (int32_t)(nspire_ms() - start) >= timeout)
			return false;
		/* the keypad is read every 10 ms; the network is polled by the
		 * scheduler, which bounds the timeout */
		nspire_idle(10);
	}
}
