# NetSurf for the TI-Nspire

NetSurf running on a TI-Nspire CX under [Ndless](https://ndless.me/), on the
web through the USB cable: while NetSurf runs, the calculator is a **USB
Ethernet adapter** for the computer it is plugged into (USB CDC NCM, which
Linux and Windows 10/11 drive without extra software), with its own TCP/IP
stack (lwIP), HTTP/1.1 and TLS 1.2/1.3 (mbedTLS).

It is the framebuffer frontend with a different surface and a few additions
(`NETSURF_FB_FRONTEND=nspire`); everything specific to the calculator lives
in this directory.

## Using it

1. Install Ndless on the calculator, copy `netsurf.tns` to it (any folder),
   open it.
2. Plug the calculator into a computer. The status bar at the bottom shows
   the network:
   - `USB: connect the cable to a computer`
   - `USB: connected, getting an address...`
   - `USB: 10.42.0.2, computer ("pc") 10.42.0.1`: ready.
3. To reach the Internet, the computer shares its connection with the new
   network interface ("NetSurf USB network", USB id 0525:a4a1):
   - **Linux** (NetworkManager): `nmcli connection add type ethernet ifname
     <the usb interface> ipv4.method shared`, or "Shared to other computers"
     in the connection's IPv4 settings.
   - **Windows 10 (2004 and later) / 11**: Control Panel → Network
     Connections → the Internet adapter → Properties → Sharing → allow other
     users to connect through this computer's Internet connection, choose the
     calculator's adapter. If the adapter shows up without a driver: Device
     Manager → Update driver → Browse → Let me pick → Network adapters →
     Microsoft → "UsbNcm Host Device".
   - macOS is not supported (it does not open the data interface of this
     device).

   Without sharing, the calculator gives the computer an address itself
   after a few seconds, and web servers on the computer are reachable as
   `http://pc/`.

The first page is a short help page (`var` brings it back).

### Keys

| Key | Does |
| --- | --- |
| touchpad (slide) | moves the pointer |
| touchpad click | click (hold: drag) |
| enter | click; while typing: go / submit |
| ctrl + slide | scroll |
| arrows | scroll the page; in a text field: move the caret |
| ctrl + up / down | page up / page down |
| menu | the address bar (empty; press again for the page's address) |
| esc | back; while typing: leave the field |
| doc | forward |
| scratchpad | reload |
| var | home page |
| ctrl + / ctrl − / ctrl 0 | zoom in / out / 100 % |
| letters, digits | type; shift for capitals |
| shift + `.` `,` `-` `+` `*` `/` `=` `(` `)` `^` `?!` | `:` `;` `_` `@` `&` `\` `#` `[` `]` `%` `!` |
| del | backspace (shift + del: delete) |
| tab | next field |
| ret | Return |
| home (on) | quit |

In the address bar, `example.com` becomes `https://example.com/`, `pc:8000/x`
and plain addresses become `http://...`, and anything else is a web search
(DuckDuckGo).

### Files next to the program

All names end in `.tns`, which TI's software requires to transfer them.

| File | |
| --- | --- |
| `netsurf_net.cfg.tns` | network settings, written on the first start (below) |
| `netsurf_choices.tns` | NetSurf options ([Choices format](../../../docs/netsurf-options.md)), optional |
| `netsurf_cookies.tns`, `netsurf_urls.tns` | cookies and history, kept between runs |
| `netsurf_log.txt.tns` | the frontend's log: network, TLS, the TI-OS stack |
| `nspire_ca.pem.tns` | extra root certificates (PEM), optional |
| `netsurf_verbose.tns` | if it exists, NetSurf's own detailed log goes to `netsurf_verbose_log.txt.tns` (slow) |

`netsurf_net.cfg.tns`:

| Key | Meaning |
| --- | --- |
| `mac` | the calculator's Ethernet address (made up once; keep it different on every calculator) |
| `mode` | `auto` (the computer's DHCP if it has one, else serve it one), `client`, `server`, `static` |
| `ip`, `netmask`, `gateway`, `dns`, `peer` | for `static` (`peer`: the computer); `ip`/`netmask` also choose the subnet of `server` |
| `dhcp_wait` | ms to wait for the computer's DHCP in `auto` (5000) |

When NetSurf exits, TI-OS gets the USB port back and the computer sees a TI
device again (TI Connect, n-link).

### Limits

- Calculators: TI-Nspire CX and CX CAS. The CX II's USB controller (Faraday
  FOTG210) has no driver yet: NetSurf runs there, the network does not.
- Experimental features are on by default: JavaScript (NetSurf's Duktape
  engine; slow on the calculator, and NetSurf's DOM support is partial),
  animated images, and international domain names shown decoded. To turn
  them off, put lines into `netsurf_choices.tns`:
  `enable_javascript:0`, `animate_images:0`, `display_decoded_idn:0`.
- 320×240 and NetSurf's built-in bitmap font: Latin, Greek, Cyrillic; no
  CJK glyphs.
- The first HTTPS connection to a site costs a TLS handshake: seconds on the
  calculator. Connections and TLS sessions are reused afterwards.
- The calculator's clock is often not set: while it is earlier than the
  build date, certificates are not rejected for their dates.
- Memory: the CX has 64 MB, shared with TI-OS; very large pages may not fit.

## Building

Linux; needs `cmake`, `gperf`, `bison`, `flex`, `perl`, `python3`, `pkg-config`,
the development files of `libpng`, `zlib`, `expat`, `libboost-program-options`.

```sh
frontends/framebuffer/nspire/scripts/sdk.sh        # the Ndless SDK into ~/nspire
. ~/nspire/env.sh
frontends/framebuffer/nspire/scripts/deps.sh        # libraries for the calculator
frontends/framebuffer/nspire/scripts/build.sh       # -> netsurf.tns
```

`deps.sh` fetches pinned versions of the NetSurf libraries and nsgenbind
(libnsfb gets the `nspire` surface from `patches/`), GNU libiconv, utf8proc,
libjpeg-turbo, mbedTLS and lwIP, and builds them into `~/nspire/netsurf-env/nspire` (`NSPIRE_NS_ENV`
moves it). The GitHub workflow `.github/workflows/nspire.yaml` does the same
and publishes `netsurf.tns` as an artifact (and as a release for `nspire-*`
tags).

## How it works

| File | Part |
| --- | --- |
| `patches/libnsfb-0001-nspire-surface.patch` | libnsfb's `nspire` surface: an RGB565 frame; showing it and input are the program's (`libnsfb_nspire.h`) |
| `surface.c` | that program side: keypad and touchpad to pointer and key events, the frame to the LCD once per pass of the main loop |
| `sys_device.c` | the machine: 32 kHz clock (also newlib's, through `--wrap=_gettimeofday`), LCD, keys, log file, 8-byte aligned heap blocks, the TI-OS task's stack in the log |
| `nspire.c` | start and end, options, built-in resources, what typed addresses mean |
| `net/usbd_ci.c` | polled device-mode driver for the CX's ChipIdea USB controller; takes it from TI-OS and gives it back |
| `net/ncm.c` | the CDC NCM function (descriptors as Linux's `f_ncm`, NTB16, Microsoft OS descriptors for Windows) |
| `net/nnet.c` | lwIP on that link: DHCP client, fallback DHCP server, DNS, non-blocking TCP |
| `net_glue.c` | the settings file, polling from NetSurf's scheduler, the status line |
| `fetch_http.c` | the `http:`/`https:` fetcher on `nnet` (there is no libcurl and no socket layer) |
| `tls.c` | mbedTLS: client configuration, the built-in roots (from `resources/ca-bundle`) looked up by issuer when a certificate is checked, so none are parsed up front |
| `scripts/mkres.py` | resources, Messages and roots into C (`nspire-resources.c`) |
| `Makefile.nspire` | sources, flags, `netsurf.tns` (genzehn + make-prg) |

The USB network code comes from the TI-Nspire port of OptiCraft (its
`src/nspire/net/`, where its design, the computer's side per OS and its tests
are described in detail).

Outside this directory: `frontends/framebuffer/gui.c` (keypad actions, the
address bar), `fetch.c` (built-in resources), and in the core, for newlib
without sockets: `utils/config.h`, `utils/inet.h`, `utils/dirent.h`,
`utils/utils.c` (`uname`), `desktop/gui_factory.c` — all under `_TINSPIRE`.
