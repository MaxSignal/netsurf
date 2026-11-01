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
 * The USB network: settings file, start and stop, polling, status.
 *
 * The calculator becomes a USB Ethernet adapter (CDC NCM) for the computer
 * it is plugged into and runs lwIP on that link (net/). With the computer
 * sharing its Internet connection, the calculator gets an address from the
 * computer's DHCP server and the whole web is reachable; without it, the
 * calculator serves the computer an address itself after a few seconds,
 * and servers on the computer ("pc") are reachable.
 *
 * netsurf_net.cfg.tns, next to the program, is written on the first start:
 *   mac=02:4e:..:..:..:..   the calculator's Ethernet address
 *   mode=auto|client|server|static
 *   ip=, netmask=, gateway=, dns=, peer=   (static; ip/netmask: server subnet)
 *   dhcp_wait=5000          ms to wait for the computer's DHCP (auto)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "utils/log.h"
#include "netsurf/misc.h"
#include "desktop/gui_internal.h"

#include "framebuffer/nspire/nspire.h"
#include "framebuffer/nspire/sys.h"
#include "framebuffer/nspire/net/nnet.h"
#include "framebuffer/nspire/net/nnet_port.h"

#define POLL_MS 20

static nnet_config cfg;
static bool started;
static bool failed;
static int last_state = -1;

static void config_path(char *buf, size_t size)
{
	snprintf(buf, size, "%s/netsurf_net.cfg.tns", nspire_app_dir());
}

static char *trim(char *s)
{
	char *e;
	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
		*--e = '\0';
	return s;
}

static bool parse_mac(const char *s, uint8_t mac[6])
{
	unsigned v[6];
	int i;

	if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
		return false;
	for (i = 0; i < 6; i++) {
		if (v[i] > 255)
			return false;
		mac[i] = (uint8_t)v[i];
	}
	return true;
}

static uint32_t ip_or_zero(const char *s)
{
	uint32_t ip = 0;
	return nnet_parse_ip(s, &ip) ? ip : 0;
}

static const char *mode_name(int mode)
{
	switch (mode) {
	case NNET_MODE_CLIENT: return "client";
	case NNET_MODE_SERVER: return "server";
	case NNET_MODE_STATIC: return "static";
	default: return "auto";
	}
}

static void load_config(void)
{
	char path[300], line[160];
	bool have_mac = false;
	const char *sim_mac;
	FILE *f;

	nnet_default_config(&cfg);
	strcpy(cfg.hostname, "netsurf-nspire");
	config_path(path, sizeof(path));
	f = fopen(path, "r");
	if (f != NULL) {
		while (fgets(line, sizeof(line), f) != NULL) {
			char *key, *value, *eq;
			key = trim(line);
			if (key[0] == '#' || (eq = strchr(key, '=')) == NULL)
				continue;
			*eq = '\0';
			key = trim(key);
			value = trim(eq + 1);
			if (strcmp(key, "mac") == 0)
				have_mac = parse_mac(value, cfg.mac);
			else if (strcmp(key, "mode") == 0)
				cfg.mode = strcmp(value, "client") == 0 ? NNET_MODE_CLIENT :
					strcmp(value, "server") == 0 ? NNET_MODE_SERVER :
					strcmp(value, "static") == 0 ? NNET_MODE_STATIC :
					NNET_MODE_AUTO;
			else if (strcmp(key, "ip") == 0)
				cfg.ip = ip_or_zero(value);
			else if (strcmp(key, "netmask") == 0)
				cfg.netmask = ip_or_zero(value);
			else if (strcmp(key, "gateway") == 0)
				cfg.gateway = ip_or_zero(value);
			else if (strcmp(key, "dns") == 0)
				cfg.dns = ip_or_zero(value);
			else if (strcmp(key, "peer") == 0)
				cfg.peer = ip_or_zero(value);
			else if (strcmp(key, "dhcp_wait") == 0)
				cfg.dhcp_wait_ms = (uint32_t)strtoul(value, NULL, 10);
		}
		fclose(f);
	}
	sim_mac = getenv("NSPIRE_SIM_NET_MAC");
	if (sim_mac != NULL)
		have_mac = parse_mac(sim_mac, cfg.mac);
	if (have_mac)
		return;

	/* first start: make up an address and keep it */
	{
		uint32_t e1 = nnet_port_entropy(), e2 = nnet_port_entropy() * 2246822519u;
		cfg.mac[0] = 0x02;
		cfg.mac[1] = 0x4E;
		cfg.mac[2] = (uint8_t)(e1 >> 24);
		cfg.mac[3] = (uint8_t)((e1 >> 16) ^ (e2 >> 8));
		cfg.mac[4] = (uint8_t)((e1 >> 8) ^ (e2 >> 24));
		cfg.mac[5] = (uint8_t)(e1 ^ e2);
	}
	f = fopen(path, "w");
	if (f != NULL) {
		fprintf(f,
			"# NetSurf USB network settings (TI-Nspire). Lines: key=value.\n"
			"# This calculator's network address (keep it different on each calculator):\n"
			"mac=%02x:%02x:%02x:%02x:%02x:%02x\n"
			"# auto: use the computer's DHCP (connection sharing) if it has one, else\n"
			"#       give the computer an address ourselves; client; server; static\n"
			"mode=%s\n"
			"# For mode=static: ip=, netmask=, gateway=, dns=, peer= (the computer)\n"
			"# ms to wait for the computer's DHCP in mode=auto:\n"
			"dhcp_wait=%u\n",
			cfg.mac[0], cfg.mac[1], cfg.mac[2], cfg.mac[3], cfg.mac[4], cfg.mac[5],
			mode_name(cfg.mode), (unsigned)cfg.dhcp_wait_ms);
		fclose(f);
	}
}

static void report_state(void)
{
	char text[128];
	int state = nnet_state();

	if (state == last_state)
		return;
	last_state = state;
	nspire_net_status(text, sizeof(text));
	nspire_log("%s\n", text);
	fb_nspire_status(text);
}

static void poll_callback(void *p)
{
	(void)p;
	nspire_net_poll();
	if (started)
		guit->misc->schedule(POLL_MS, poll_callback, NULL);
}

void nspire_net_poll(void)
{
	uint32_t start;

	if (!started)
		return;
	nnet_poll();
	/* Until the computer has the link up, answer it for a while each
	 * time: it enumerates the device with a few dozen control requests
	 * and expects each answered promptly. */
	if (nnet_state() == NNET_WAIT_USB) {
		start = nspire_ms();
		while ((uint32_t)(nspire_ms() - start) < 15)
			nnet_poll();
	}
	report_state();
}

void nspire_net_start(void)
{
	if (started || failed)
		return;
	load_config();
	if (getenv("NSPIRE_NO_NET") != NULL || nnet_start(&cfg) != 0) {
		failed = true;
		nspire_log("net: not started\n");
		fb_nspire_status("USB network: this calculator's USB controller is not supported");
		return;
	}
	started = true;
	report_state();
	guit->misc->schedule(POLL_MS, poll_callback, NULL);
}

void nspire_net_stop(void)
{
	if (!started)
		return;
	started = false;
	guit->misc->schedule(-1, poll_callback, NULL);
	nnet_stop();
}

void nspire_net_status(char *buf, int size)
{
	if (failed) {
		snprintf(buf, (size_t)size, "USB network: not available on this calculator");
		return;
	}
	if (!started) {
		snprintf(buf, (size_t)size, "USB network: off");
		return;
	}
	nnet_status_text(buf, size);
}
