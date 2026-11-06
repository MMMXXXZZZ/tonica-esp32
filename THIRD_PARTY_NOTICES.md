# Third-party notices

Tónica and Listenica are MIT-licensed (see `LICENSE`) and build on the open-source software below. The full licence texts are in
[`licenses/`](licenses/), copied from each project's own files. The same notices are published with
the firmware at https://tonica.m44.cl/licenses.txt.

Listenica implements Spotify's protocols itself (clean room). It was modelled on how
[librespot](https://github.com/librespot-org/librespot) (MIT) and
[Spotifast](https://github.com/crmne/spotifast) (MIT) work, but contains no code from them.

## In the Tónica firmware (ESP32-S3)

| Component | Version | Copyright | Licence | Text |
|---|---|---|---|---|
| ESP-IDF (drivers, Wi-Fi, USB host, NVS, HTTP server, OTA, …) | 5.5.1 | Espressif Systems | Apache 2.0 | `Apache-2.0.txt`, `ESP-IDF-COPYRIGHT.rst` |
| Espressif Wi-Fi/PHY libraries (incl. FreeBSD net80211) | 5.5.1 | Espressif Systems; Sam Leffler, Errno Consulting | Apache 2.0; BSD | `Apache-2.0.txt`, `ESP-IDF-COPYRIGHT.rst` |
| FreeRTOS kernel | (ESP-IDF 5.5.1) | Amazon.com, Inc. or its affiliates | MIT | `FreeRTOS-Kernel-MIT.md` |
| lwIP TCP/IP stack | 2.x (ESP-IDF 5.5.1) | Swedish Institute of Computer Science | BSD 3-Clause | `lwIP-BSD-3-Clause.txt` |
| Mbed TLS | 3.6.4 | The Mbed TLS Contributors (ARM Limited) | Apache 2.0 (chosen of Apache 2.0 OR GPL-2.0-or-later) | `MbedTLS-LICENSE.txt`, `Apache-2.0.txt` |
| cJSON | 1.7.18 | Dave Gamble and cJSON contributors | MIT | `cJSON-MIT.txt` |
| newlib C library | (ESP-IDF toolchain) | various | BSD and similar | `newlib-COPYING.txt` |
| wpa_supplicant | (ESP-IDF 5.5.1) | Jouni Malinen and contributors | BSD | `wpa_supplicant-BSD.txt` |
| http_parser | (ESP-IDF 5.5.1) | Igor Sysoev; Joyent, Inc. and other Node contributors | MIT | `http_parser-MIT.txt` |
| TLSF allocator | (ESP-IDF 5.5.1) | Matthew Conte | BSD 3-Clause | `TLSF-BSD-3-Clause.txt` |
| Xtensa HAL headers | (ESP-IDF 5.5.1) | Tensilica Inc. | MIT | `ESP-IDF-COPYRIGHT.rst` |
| espressif/mdns | 1.14.1 | Espressif Systems | Apache 2.0 | `Apache-2.0.txt` |
| espressif/led_strip | 3.1.1 | Espressif Systems | Apache 2.0 | `Apache-2.0.txt` |
| espressif/usb_host_uac (vendored, **modified**: see `tonica/components/usb_host_uac/MODIFIED.md`) | 1.5.0 | Espressif Systems | Apache 2.0 | `tonica/components/usb_host_uac/LICENSE` |
| Mozilla CA certificate list (as data, in ESP-IDF's certificate bundle) | 2025-02-25 | Mozilla | MPL 2.0 | source: https://curl.se/docs/caextract.html |
| stb_vorbis (Ogg Vorbis decoder) | 1.22 | Sean Barrett and contributors | MIT or public domain (Unlicense) | `stb_vorbis-MIT-or-Unlicense.txt` |
| tiny-AES-c | — | kokke and contributors | Unlicense (public domain) | `tinyaes-Unlicense.txt` |
| puff (inflate) | 2.3 | Mark Adler | zlib | `puff-zlib.txt` |
| Bodoni Moda (subset, in the setup page) | — | The Bodoni Moda Project Authors | SIL Open Font License 1.1 | `Bodoni-Moda-OFL-1.1.txt` |

The GCC runtime (libgcc, libstdc++) is covered by the GCC Runtime Library Exception and carries no
notice requirement.

## In the Listenica host (Windows)

Mbed TLS, cJSON, stb_vorbis, tiny-AES-c and puff, as above. It also uses Windows system libraries.

## On the website (tonica.m44.cl)

- Bodoni Moda and Hanken Grotesk fonts: SIL Open Font License 1.1 (served with their licence files in `/fonts`).
- esptool-js 0.7.0 (Apache 2.0), loaded from jsDelivr, not redistributed by the site.
- The board drawing's lettering uses the Hershey Simplex strokes (Dr. A. V. Hershey, U.S. National
  Bureau of Standards).
