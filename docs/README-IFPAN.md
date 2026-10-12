# piHPSDR-IF — IF panadapter and remote front end for the Kenwood TS-570D

piHPSDR-IF is piHPSDR (DL1YCF) built with `IFPAN=ON` by CU2ED: an SDR
receives the **first IF (73.05 MHz)** of a **Kenwood TS-570D**, and the
program becomes the radio's panadapter, second receiver, TX audio
processor, CAT remote control and remote station (client / server).

Repository: https://github.com/jacintomfr/pihpsdr (a fork of
dl1ycf/pihpsdr). Nothing of this work has been offered upstream.

## Documents

| Document | For |
|---|---|
| **`docs/IFPAN-USER-GUIDE.md`** | operating it: menus, toolbar, tuning, TX, RADE, client/server |
| **`docs/IFPAN-GUIA-UTILIZADOR.md`** | the same user guide in Portuguese (guia do utilizador em português) |
| **`docs/IFPAN-INSTALL-LINUX.md`** | Linux / Raspberry Pi 5 from scratch (packages, MSi2500 clone with SoapyMiri, RADE, build, launcher, CAT bridge service, server, troubleshooting) |
| **`docs/IFPAN-INSTALL-WINDOWS.md`** | Windows: MSI, Zadig drivers, CAT bridge (WSL + usbipd), audio, server/client — and building the MSI |
| `docs/RADE-IFPAN.md` | FreeDV RADE V1 inside piHPSDR-IF (maintainer reference) |
| `docs/IFPAN-WORKLOG.md` | full development log: every feature, commit, measurement, finding, open item |
| `ifpan-tools/README.md` | helper scripts and test programs (CAT probes, network loss test, MSi2500 tests, Pi / WSL service files) |
| `ESP32/ts570_wifi_bridge/README.md` | ESP32 WiFi ↔ RS-232 CAT bridge firmware |

## Branches

| Branch | Content |
|---|---|
| **`cu2ed`** | main line: Linux / Raspberry Pi and all shared code, docs, `ifpan-tools/` |
| **`windows-port`** | `cu2ed` + the Windows build (`WINDOWS/`, MSI installer); `cu2ed` is merged into it after every change |
| `stable-3.1.118`, `stable-3.1.133`, `stable-3.1.165` (+ `stable-win-*`) | known-good bookmarks (MSI version numbers) |
| `sdrplay`, `windows-sdrplay` | where the MSi2500 / SDRplay-clone work started (merged) |
| `master` | upstream piHPSDR, untouched |

Versions: 3.1.N, N = commits since the v3.1 tag (no own tags). Server and
client must be the same build.

## Station (CU2ED, 2026-10)

```
                 antenna
                    |
             Kenwood TS-570D ---- ACC2 audio in <-- 600 ohm transformer <-- sound card (TX audio)
               |        |
   1st IF 73.05 MHz   RS-232 CAT (57600) -- FTDI USB adapter -- socat bridge (TCP 127.0.0.1:7373)
   via IF buffer            |
               |            |
   MSi2500 / RSP1A clone (USB, SoapyMiri)      MIDI VFO (USB)
               \            |                /
                 Raspberry Pi 5  — piHPSDR-IF **server** (also usable on its own screen)
                            |
                          LAN (UDP audio + spectrum, TCP control)
                            |
                 Windows / Linux PC — piHPSDR-IF **client** (speaker, microphone)
```

The Windows PC can also be the server (it was during development); with
the MSi2500 clone it suffers a ground-loop stall at ≥ 25 W that the Pi does
not have (Windows guide, A6).

## What works (confirmed by the user on the radio)

* IF panadapter centred on the Kenwood VFO; two-way CAT; "Follow Kenwood",
  "Clicks tune the TS", "Sync mode with TS"; display slides while the
  Kenwood follows a tuning step (MIDI VFO / wheel), 96k and 384k.
* Band / mode / preamp / attenuator / antenna tuner / speaker mute (TS MUTE)
  from piHPSDR-IF; Kenwood S-meter, power, SWR, ALC.
* TX: WDSP-processed microphone to ACC2 (local and remote, clean), TX
  Drive = Kenwood PWR in 5 W steps (also from a client), VOX, mic RNNoise,
  TUNE and TwoTone tones (heard at the sound card output), SWR protection.
* Client / server mirror of everything incl. RADE; latency 0–1 ms.
* RADE V1 RX on air (server and client).
* MSi2500 / SDRplay RSP1A clone via SoapyMiri (Windows and Pi): DC spike
  and mirror moved out of the 384k span, LNA off option, IF offset per
  mode (LSB −3000 Hz).
* Raspberry Pi 5 server: 100 W SSB without the SDR stall.

## Open (see the worklog, section 5)

* ACC2 on-air tests (TUNE / TwoTone power, ALC, FM deviation, SWR
  protection in TUNE, **RADE TX**) — the ACC2 plug was missing.
* LAN packet loss between the PC and the Pi (client audio cuts): cables /
  switch being replaced.
* IF offset for CW / CW-R / AM / FM; RADE FreeDV Reporter; RADE CPU on the
  Pi; ESP32 CAT bridge on hardware.
