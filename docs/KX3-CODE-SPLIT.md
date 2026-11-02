# piHPSDR-KX3 — how the code is split (phase 1 design)

Status: **proposal, to be approved by the user before any function is changed.**
Phase 1 itself only added the build flag and the program name (below).

## 1. Principle

piHPSDR-KX3 is piHPSDR-IF with a second radio. It is built with
**`IFPAN=ON` + `KX3=ON`**: all the "rig front end" code of piHPSDR-IF stays in
use (CAT thread, TX through a sound card, protections, client / server, touch
UI, RADE, SSTV, clean audio, Record WAV, Capture / Replay, mic NR), and only what
differs on the KX3 goes into `#ifdef KX3` blocks inside that code.

Why not a separate copy of the files: the TS-570D build stays buildable from the
same sources (`KX3=OFF`), every improvement made in `cu2ed` merges into `kx3`
with few conflicts, and `kx3` could later be merged back so one tree builds both
programs. Upstream piHPSDR files are touched only where IFPAN already is.

## 2. Done in phase 1

| What | Where |
|---|---|
| `KX3=ON` option (default ON on the `kx3` branch), requires `IFPAN=ON`, adds `-D KX3` | `Makefile` |
| Program `pihpsdr-kx3`; app name **piHPSDR-KX3**; settings `~/.config/pihpsdr-kx3`; start script `pihpsdr-kx3.sh` — installs next to piHPSDR-IF without sharing settings | `Makefile`, `src/appname.h` |
| Checked with `make -n`: KX3=ON → `-D KX3`, `-o pihpsdr-kx3`; KX3=OFF → `pihpsdr-if`; IFPAN=OFF with KX3=ON → refused | — |

Still to rename in later phases (no effect until then): Linux installer
(`LINUX/libinstall.sh` desktop entry), Windows branch (`kx3-windows`, worktree
`Documents/pihpsdr-kx3-win`), MSI name and `%LOCALAPPDATA%` folder
(`src/startup.c` on the Windows branch), About text.

## 3. Area by area

| Area | piHPSDR-IF (TS-570D) | piHPSDR-KX3 | How |
|---|---|---|---|
| **RX input** | SDR (SoapyMiri) tuned to the 73.05 MHz IF + LO offset 250 kHz | KX3 RX I/Q into a USB sound card, read as a SoapySDR **audio** device (first choice — reuses the whole Soapy receive path, DC removal, reopen logic) | Discovery shows the sound card as a Soapy device. Fallback if the audio module fails on the Pi 4 (latency, 192k, channel order): a small native I/Q reader in a new `kx3_iq.c`. |
| **Frequency mapping** | `ifpan_hw_frequency()` = IF + mode IF offset + LO offset | IF = 0, LO offset = 0; the per-mode offset is kept in case the KX3 I/Q has an offset per mode (to measure in phase 3) | `#ifdef KX3` defaults in `ifpan.c`; the Soapy audio device's frequency is only a label |
| **Display centre** | the Kenwood VFO by CAT, slides while the radio follows | the same with the KX3 VFO | no change (`centre_apply`, `ifpan_display_shift`) |
| **IQ balance / image** | not needed (SDR) | maybe needed (KX3 + sound card) | measure in phase 3; if needed, a small IQ phase/gain correction where the DC removal is (`soapy_protocol.c`, IFPAN block) |
| **CAT transport** | Kenwood TCP 127.0.0.1:7373 (socat to FTDI) or rigctld | the same transport: socat to the KXUSB serial port (or a WiFi bridge), or rigctld | no change |
| **CAT dialect** | `kw_*` (TS-570) behind the `be_*` backend functions | new **Elecraft** set `el_*` behind the same `be_*` (KX3 commands verified in the Programmer's Reference and on the radio) | `#ifdef KX3` in `be_*`; `kw_*` kept for the TS build |
| **Modes** | `kenwood_mode_index` (DIGU/DIGL → USB/LSB) | KX3 `MD` codes + `DT` data sub-mode (DATA A for RADE / SSTV / digital) | KX3 table under `#ifdef KX3` |
| **Power / drive** | `PC` 5 W steps, 5–100 W, RADE limit 50 W | KX3 scale (0–15 W, 0.1 W steps? / KXPA100 up to 100 W) — from the reference | `clamp_watts`, `ifpan_set_power`, RADE limit under `#ifdef KX3` |
| **Meters** | `SM` in TX = power, `RM` SWR / ALC | KX3 equivalents (to find in the reference) | `kw_tx_meters` → `el_tx_meters` |
| **Preamp / attenuator** | `PA`, `RA` | KX3 `PA` / `RA` if they match | backend |
| **ATU** | `AC` (TS tuner) | KX3 ATU (KXAT3) if fitted — by CAT if available | backend; hidden if not |
| **TS MUTE (AG)** | TS speaker mute | KX3 AF gain mute, if useful | backend |
| **Split / subtone** | `FT`, `FB`, `TO`, `TN` | KX3 split / VFO B by CAT | backend |
| **Radio TX EQ** | `EX014000n` (SSB choice, OFF in SSTV / RADE) | KX3 TX EQ by CAT if available, same rule | backend |
| **TX audio** | sound card → transformer → ACC2 pin 11, PTT by CAT | sound card → KX3 MIC jack (LINE IN) or DATA A; PTT by CAT | no code change expected; docs + menu labels ("TX audio to KX3") |
| **Protections** | SWR alarm, TX time-out | the same | no change |
| **Menus / toolbar** | Radio menu "Kenwood / IF panadapter" block, TS functions in the toolbar | "KX3" block: no IF offset / LO / SDR LNA rows; TS-only actions hidden (like `ifpan_hidden_action`) | `#ifdef KX3` in `radio_menu.c`, `toolbar.c`, `actions.c` |
| **Touch UI, RADE, SSTV, clean audio, Record WAV, Capture / Replay, mic NR, MIDI VFO, client / server, packet-loss readout** | as built | the same | no change (only labels saying "TS" become the radio name) |
| **Labels** | "TS", "Kenwood" in VFO bar, meters, alerts | "KX3" | a `RIG_NAME` macro in `appname.h` instead of literal "TS" strings |

## 4. Order of work (phases of the brief)

1. ~~Set-up~~ (this document).
2. Manuals (user's OK to download the KX3 Owner's Manual and the K3/KX3
   Programmer's Reference to `Documents/manuals`, never committed).
3. RX only on the Pi 4: Soapy audio device, span, DC, I/Q swap, image rejection,
   CAT read of the VFO for the centre.
4. Elecraft CAT backend (`el_*`): tuning both ways, mode, filter, preamp /
   attenuator, power, meters.
5. TX into the KX3 (dummy load, user's OK per test).
6. Client / server, RADE, SSTV, Record WAV, touch on the Pi 4 screen; Windows
   branch and MSI; docs.
