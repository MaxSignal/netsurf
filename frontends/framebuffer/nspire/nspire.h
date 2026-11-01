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
 * TI-Nspire frontend: what gui.c calls, and what it provides in return.
 */

#ifndef NETSURF_FB_NSPIRE_H
#define NETSURF_FB_NSPIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "utils/errors.h"

struct gui_fetch_table;

/** Machine setup, before anything else in main(). */
void nspire_early_init(int *argc, char ***argv);

/** Load the built-in Messages. */
nserror nspire_messages_init(void);

/** After netsurf_init(): option overrides, the network, its fetchers. */
nserror nspire_init(void);

/** Before netsurf_exit(): give the USB controller back, save state. */
void nspire_finalise(void);

/** Last thing in main(). */
void nspire_late_finalise(void);

struct nsurl;
/**
 * What was typed in the address bar: a URL, a host ("example.com",
 * "pc:8000/x", "10.0.0.2"), or words to search for.
 */
nserror nspire_url_from_text(const char *text, struct nsurl **url);

/** Where to look for resource files on disk (the program's folder). */
const char *nspire_app_dir_respath(void);

/** The path to the Choices file. */
const char *nspire_choices_path(void);

/** Built-in resources: resource:<path>. */
nserror nspire_get_resource_data(const char *path, const uint8_t **data, size_t *len);
nserror nspire_release_resource_data(const uint8_t *data);

/* Actions gui.c carries out for the keypad (gui.c). */
enum nspire_action {
	NSPIRE_ACTION_BACK,
	NSPIRE_ACTION_FORWARD,
	NSPIRE_ACTION_RELOAD,
	NSPIRE_ACTION_URLBAR,
	NSPIRE_ACTION_HOME,
	NSPIRE_ACTION_UNFOCUS,
};
void fb_nspire_action(enum nspire_action action);
/** A text field (URL bar or form) has the caret. */
bool fb_nspire_text_input_active(void);
/** Put a line in every window's status bar. */
void fb_nspire_status(const char *text);
/** The rectangle the page is shown in (screen coordinates). */
bool fb_nspire_browser_area(int *x0, int *y0, int *x1, int *y1);

/* Network glue (net_glue.c) */
void nspire_net_start(void);
void nspire_net_stop(void);
void nspire_net_poll(void);
/** One line describing the USB network. */
void nspire_net_status(char *buf, int size);

/* HTTP/HTTPS over the USB network (fetch_http.c) */
nserror nspire_fetch_http_register(void);

/* TLS (tls.c) */
struct mbedtls_ssl_config;
/** Shared client configuration; NULL if TLS cannot work. */
const struct mbedtls_ssl_config *nspire_tls_config(bool verify);
void nspire_tls_finalise(void);

#endif
