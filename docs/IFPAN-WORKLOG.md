# piHPSDR-IF — work log and state (CU2ED)

Detailed record of what has been built in this fork, why, how it was
verified, and what is still open. Kept up to date with the work; the commit
messages hold the per-change detail (hashes below are on `cu2ed` unless
marked `windows-port`). RADE has its own document: `docs/RADE-IFPAN.md`.

Last update: 2026-10-11 (SDRplay work merged into the main line).

## 1. What piHPSDR-IF is

A build of piHPSDR (`make IFPAN=ON`, binary `pihpsdr-if`, name in
`src/appname.h`, settings in `~/.config/pihpsdr-if` / `%LOCALAPPDATA%\piHPSDR-IF`)
used as a panadapter and remote front end for a **Kenwood TS-570D**:

* An SDR (first a PlutoSDR, now an **RTL-SDR R820T with TCXO**) receives the
  TS-570D's **1st IF (73.05 MHz)**; the display is centred on the Kenwood's
  VFO, read by CAT. The SDR never transmits.
* The Kenwood is controlled by **CAT** (frequency, mode, PTT, preamp,
  attenuator, antenna tuner, output power, meters).
* **TX audio**: the WDSP TX chain processes the microphone (local, or the
  remote client's) and plays it on a sound card wired through an isolation
  transformer into the TS-570D **ACC2** input.
* **Client / server**: the server (planned: Raspberry Pi 4 next to the
  radio) owns the radio; the client (Windows / Linux) is the operator's
  station — a mirror of everything, with microphone and speaker.
* **RADE V1** digital voice runs on the server (`docs/RADE-IFPAN.md`).

Repositories / branches:

| Where | Branch | Purpose |
|---|---|---|
| `Documents/pihpsdr-cu2ed` | `cu2ed` | main line (Linux + shared code) |
| `Documents/pihpsdr-cu2ed-win` (worktree of the same repo) | `windows-port` | Windows build + MSI; `cu2ed` is merged into it after every change |
| bookmarks | `stable-3.1.118`, `stable-win-3.1.118` | known-good state of 2026-10-11 (MSI 3.1.118) |
| work branches | `sdrplay`, `windows-sdrplay` | SDRplay RSP1A-clone work (not started) |

Never tag: the MSI version comes from `git describe` (3.1.N, N = commits
since the v3.1 tag). Both worktrees share one repository, so branch names
must differ between them.

## 2. Station setup (Windows PC, 2026-10)

* **CAT**: FTDI USB-RS232 adapter (0403:6001) on the TS-570D at **57600 baud**
  (radio menu 35 = "576-1", needs a radio power cycle). It is attached to
  WSL (Ubuntu-24.04) with usbipd; systemd service **`ts570-cat.service`**
  runs `/usr/local/bin/ts570-cat-bridge.sh` =
  `socat TCP4-LISTEN:7373,reuseaddr,fork FILE:/dev/ttyUSB0,b57600,raw,echo=0`;
  Windows reaches it at **127.0.0.1:7373** ("CAT via Kenwood TCP"). A
  Startup-folder `ts570-cat-bridge.vbs` keeps the distro alive and runs
  `usbipd attach --auto-attach` for 0403:6001.
  The FTDI adapter caused interference lines on the panadapter; an ESP32-S3
  WiFi bridge firmware exists (`ESP32/ts570_wifi_bridge`, TCP 7373, needs a
  3.3 V MAX3232) — compiled, not yet on hardware.
* **RTL-SDR** (0BDA:2838) on WinUSB (Zadig), used through SoapyRTLSDR (in
  the MSI). Frequency calibration: Radio menu (Hz per 10 MHz), applied to
  the IF LO.
* **Sound cards**: ASUS Xonar Essence STX + motherboard audio; the TX audio
  output to ACC2 is chosen in the Radio menu ("TX audio to TS", server only).
* Server and client currently both run on this PC (client connects to
  127.0.0.1:50000).
* **SDRplay RSP1A clone** with TCXO: USB 1DF7:2500 (MSi2500, seen as an RSP1),
  plugged in on 2026-10-11, no driver yet (problem code 28).

TS-570D facts verified on the radio (only probe with exclusive access: the
radio drops CAT commands that arrive while it answers another client):

| Command | Behaviour |
|---|---|
| `IF;` | position 28 = TX flag, 29 = mode |
| `SM;` in TX | power: 5 W per dot to 50 W, then 10 W per dot (100 W = 15) |
| `RM;` | reads only the selected meter; `RM1;` / `RM3;` select SWR / ALC; SWR = 60/(30−n)−1 |
| `MDn;` | mode; must be sent ~300 ms after a band-changing `FA` and read back |
| `PA0;` / `PA1;` | preamp; `RA00;` / `RA01;` attenuator |
| `AC;` | → `AC` + RX-AT + TX-AT + tuning (e.g. `AC010`); `AC 10;` / `AC 00;` tuner in / through (relay heard); `AC 11;` starts tuning |
| `PC;` / `PCnnn;` | output power: 5 W steps, 5..100 W, AM 5..25 W, kept per mode; 10 / 20 W gave SM 2 / 4 on a dummy load |
| `TX;` / `RX;` | PTT |

## 3. Build and release procedure

* Linux check build in WSL: `~/pihpsdr-cu2ed` (sources copied from the
  Windows tree, `dos2unix`, `make -j8`; scratch script `build4.sh`).
* Windows: MINGW64 shell, `WINDOWS/installer/build-exes.sh && stage.sh &&
  build-msi.sh` in `pihpsdr-cu2ed-win` → `build-win/piHPSDR-IF-3.1.N-x64.msi`.
* Every change: commit on `cu2ed` (English message, `Co-Authored-By`
  line), `git merge --no-edit cu2ed` into `windows-port`, build the MSI, the
  user installs it on both PCs and tests.
* RNNoise model (`rnnoise/src/rnnoise_data.c`, ~78 MB) is not in git; the
  Makefile downloads it by `rnnoise/model_version`.
* RADE libraries: `./build-rade.sh` (or copy `deskhpsdr/rade-libs` on
  Windows); see `docs/RADE-IFPAN.md` section 6.
* TX diagnostics: an empty file `ifpan-txdump` in the settings folder at
  server start makes it record `txdump_mic.f32` / `txdump_out.f32` (30 s,
  float32, 48 kHz) while transmitting.

## 4. Features (in order of development)

### 4.1 IF panadapter core (2026-10-10)
* `4088178e` receive-only IF panadapter build (PlutoSDR first).
* `b511d5ae`, `3e7a138c` Pluto LO offset (+50 kHz) to move the DC spike, IF
  shifted to DC on raw samples before the WDSP resampler.
* `c6bcb885`, `de97df82`, `16e2e6ab` Hamlib model 2004, Swap IQ off (the
  TS-570D 1st IF is inverted), CAT 57600, rigctld cache off.
* `4d13dd7d` two-way CAT; `7150f4ff` "Follow Kenwood"; `52361c63`,
  `8996821a` window position memory.
* `f05dcdf9` Soapy RX backs off on read errors (was a 2 GB log in minutes).
* `ba55e0d9`, `32e44b22` Kenwood mode next to "TS", TS-570D S-meter.
* `2ec146ab` ESP32 WiFi bridge firmware; `2527f2d4` CAT via Kenwood TCP
  (no rigctld).
* `windows-port` `674d48d0`..`c8ef5c48`, `b8fa7c3d`: POSIX compat layer,
  MinGW64 Makefile section, settings folder, PortAudio latency, MSI (WiX v3),
  RTL-SDR in the MSI, DLL closure from PE imports, working
  `clock_nanosleep` (absolute sleeps returned at once on MinGW).

### 4.2 TX state, meters, VFO bar, PTT (2026-10-10)
* `c1628887` TX state from `IF;`; Radio menu "Mute when TS in TX"; PWR /
  SWR / ALC read by CAT.
* `7188df26`, `45e84515` Kenwood power / SWR on piHPSDR's own main TX meter.
* `a5653e57`, `350eb0b8` VFO bar rows (row 1 mode / TS, row 2 NR..RIT..PRE
  ATT, row 3 Locked..Step / AGC); RIT moved to row 2 (did not fit on the
  Windows font).
* `5567374a` toolbar PTT / MOX key the Kenwood by CAT ("RX;" on exit).

### 4.3 Display while the Kenwood tunes, tuning helpers (2026-10-10/11)
* `10fb8fd1`, `450f07bd`, `1d1ac6f4` display average follows the signals
  while the dial turns: new WDSP function `ShiftDisplayAverage`
  (`wdsp/analyzer.c`). Spectrum sliding while the dial turns is physics (the
  Kenwood's LO moves).
* `77d6c09c` Radio menu "Clicks tune the TS" (off = still spectrum, mouse
  tuning inside it).
* `c7d14118` toolbar "TS here" (tune the Kenwood to the RX frequency).
* `fe3cd569`, `96a9ec4d` "Sync mode with TS", both ways.
* `21b74ddd` SDR reopen when it drops off (RTL unplugged → back in ~1.5 s).
* `2289c765` spectrum-correlation "Track tuning" tried and **reverted**
  (`b5f18f3d`): the user judged it not worth the code — do not re-propose.

### 4.4 Band, mode, preamp, attenuator (2026-10-11)
* `fcc73916`, `b78b6db6`, `1b17ccf6` band menu shows the TS-570 bands
  (100 kHz–60 MHz) and sends FA + band-stack mode; MD sent 300 ms later,
  verified, retried.
* `f38cada0` toolbar PRE = Kenwood preamp; `b2a16400` toolbar ATT = Kenwood
  attenuator; both shown in the VFO bar.

### 4.5 Client / server mirror (2026-10-11)
* `3d8b13c3` INFO_IFPAN (server → client, UDP with the periodic data, 150 ms:
  Kenwood state + Radio-menu options) and CMD_IFPAN (client → server: PTT,
  MOX, TS here, PRE, ATT, ATU, options). The client opens no CAT link. Own
  protocol version (now 0x013100F3): both ends must run the same build.
* `73a18c55` mode / filter changes made on the server (Kenwood mode, RADE,
  Fit Filter, Quick Tune) are pushed to the client
  (`ifpan_push_vfo_to_client`) — confirmed.

### 4.6 TX audio to the Kenwood (2026-10-11)
* `581abe45` the transmitter object is created in IFPAN: WDSP TX chain at
  48 kHz, the real part of the output is the processed audio for ACC2;
  `radio_set_mox` keys the Kenwood by CAT (refused without CAT).
* `19fe485e` processed as SSB; later `69f744df`: AM / FM processed as **DSB**
  (symmetric TX display, still audio) and the +15 dB FM mic boost skipped.
* `c76ec40f` TX diagnostics dump.
* Upstream bugs found and fixed while making remote TX audio clean (good
  candidates to offer to DL1YCF):
  * `windows-port b8fa7c3d` MinGW `clock_nanosleep` ABSTIME returned −1 at once.
  * `cf02c06d` client started a new remote TX audio thread on every
    INFO_TRANSMITTER.
  * `3eedea65` PortAudio mic reader could pass the writer (MIC_PRIME).
  * `d7019a63` PortAudio mic buffer reset every other callback in RX
    (VOX / mic level dead).
  * `959b9b64` client VOX: the MOX echo cancelled the VOX hang (MOX stuck).
  * `d610b98a` client latency: PING answered via the GUI loop (0→32 ms
    jumps), Windows `gettimeofday` 15.6 ms steps, 15/16-bit mask mismatch
    — now 0–1 ms, confirmed.
* Confirmed by dump analysis: remote TX audio clean (2 jumps in 4 s vs ~170/s).

### 4.7 Mic noise reduction and VOX (2026-10-11)
* `f3f107c8` RNNoise (xiph, identical to hpsdr-rs) on the client before VOX
  (TX menu, DEXP page "Mic Noise Reduction (NR3)"; `src/mic_nr.c`,
  `rnnoise/`). User: "super".
* `959b9b64` VOX stays live while the VOX menu is open (user requirement).
  Confirmed 100 %.

### 4.8 Antenna tuner, status line (2026-10-11)
* `4b6cab6a` toolbar ATU / ATU tune; smaller, lower dial with a status line
  (ATU ON / THRU / TUNING). Confirmed incl. a tuning cycle.
* `e8349994` reconnect after 3 s of CAT silence (USB adapter re-enumeration).
* `2cf2d310` client latency moved onto the status line.

### 4.9 Power, protection, TUNE, TwoTone (2026-10-11)
* `a1ec6f1c` **SWR protection** from the Kenwood SWR (TX menu "SWR alarm
  at" / "SWR Protection"): two readings at or above the alarm → `RX;` at
  once, MOX off, drive 0; skipped while the ATU tunes; in TUNE after 10 s.
* `0111f9db` **TX Drive = Kenwood PWR** by `PCnnn;`, slider follows the
  radio (1.5 s hold after a slider move); ACC2 audio now fixed level; drive
  0 = no ACC2 audio. Confirmed incl. on the client.
* `2a385c83` drive and tune power in 5 W steps (`DRIVE_STEP`).
* `0bb24b11` **TUNE**: 1 kHz tone to ACC2, own "Tune power (W)", PWR
  restored afterwards; CW / FSK switched to USB / LSB for the tune.
* `13632c55` **TwoTone** at TX Drive power, same mode switch; after a
  tune the TX Drive power is set again. Tones confirmed on the server's
  sound card output.

### 4.10 RADE V1 (2026-10-11) — see `docs/RADE-IFPAN.md`
* `676943d9` phase 1: engine on the server (ported from the deskHPSDR
  Windows RADE port); DIGU / DIGL go to the Kenwood as USB / LSB.
  **RX confirmed on air** (14.236 MHz, SYNC ~21 dB SNR, EOO callsign CU2FO,
  audio very good).
* `36421b40` phase 2: the client's RADE menu controls the server's engine
  (INFO_RADE / CMD_RADE) — confirmed.

### 4.11 Window position fix (2026-10-11)
* `4306cb42` a minimised start made the restore "compensate" Windows'
  fictitious position and save 16030,16012: every start put the window out
  of sight. Positions are now only used / saved on a monitor.

### 4.12 SDRplay RSP1A clone (MSi2500 + MSi001, USB 1DF7:2500) (2026-10-11)
Branches `sdrplay` / `windows-sdrplay`. Chosen path: **SoapyMiri +
libmirisdr-5** (GPL, shipped in the MSI) instead of the closed SDRplay API
(the old 3.07 that suits clones is no longer offered; 3.09+ give RSP1s a
random serial; a clone failed verification with 3.06–3.09 elsewhere).

What was found on this PC (all tests with exclusive access to the device):

* **Driver: libusbK** (Zadig). With **WinUSB** the device never streamed:
  bulk mode gave only timeouts (`async transfer status 2`), even freshly
  plugged; isochronous mode crashed libusb in its event thread.
* **Isochronous transfers.** SoapyMiri's default is bulk; libmirisdr's own
  example uses isochronous when built with MinGW. With libusbK +
  isochronous: 1.535 MS/s, no timeouts / overflows / errors, and three
  close / reopen cycles **without replugging** (the "restream needs a
  replug" problem reported upstream is a bulk-mode one).
* **Old libusb in `C:\Windows\System32`** (1.0.24, from another program,
  2023) is loaded before MSYS2's 1.0.30 by test programs; it lacks the
  needed isochronous support. The MSI is not affected (`libusb-1.0.dll`
  1.0.30 sits next to `pihpsdr-if.exe`).
* USB descriptors: interface 0, alt 1 = isoc 3 x 1024, alt 2 = isoc 1024,
  alt 3 = bulk 512, endpoint 0x81, high speed.

Changes:
* `939d629a` 1536k hardware rate for the `soapyMiri` driver (its minimum is
  1.3 MS/s; 14-bit format at this rate).
* `4257eb9b` RX streams (also the IFPAN reopen) ask SoapyMiri for
  `transfer=ISOC` (`rx_stream_args()` in `soapy_protocol.c`).
* `windows-sdrplay`: `build-deps.sh` builds libmirisdr-5 (library only, its
  CLI tools do not build with MinGW) and SoapyMiri with
  `WINDOWS/installer/soapymiri-transfer-mode.patch` (SoapyMiri kept the
  transfer mode as a pointer into setupStream's arguments, read again in
  activateStream); `stage.sh` ships `libsoapyMiriSupport.dll`
  (`libmirisdr.dll` through the DLL closure).
* `47e62de1` the MSi001's strong DC spike (SoapyMiri has no DC correction)
  appeared at −LO offset with a weaker mirror at +LO offset, fixed on the
  display whatever the Kenwood VFO: one-pole DC removal before the IF
  rotation, and **"SDR LO offset (kHz)"** in the Radio menu (0..250 kHz,
  also from a client; protocol 0x013100F4).

Result (user, MSI 3.1.127–3.1.129): works, audio noticeably better than the
RTL-SDR ("equal to the Hermes-Lite 2"); with **250 kHz offset at 384k**
spike and mirror are out of sight. At 768k they come back weaker — an
offset above 384 kHz would need a 3.072 MS/s hardware rate (aliasing at
1.536 MS/s); the user stays at ≤ 384k. Recommended set-up: libusbK,
isochronous, 1.536 MS/s, LO offset 250 kHz, USB3 port. Close / reopen in
the program confirmed by the user; merged into `cu2ed` / `windows-port`
on 2026-10-11 (no long-run report yet).

## 5. Open items (2026-10-11)

1. **ACC2 cable** (user is looking for the plug). Then test on a dummy load:
   TUNE / TwoTone power, ALC level (set by the sound card volume), CW → USB
   switch during TUNE, FM deviation, SWR protection during TUNE, and
   **RADE TX** (TS-570D PROC off, TX EQ flat, ALC low; PTT held ~0.5–2 s
   for the EOO).
2. **RADE phase 3**: FreeDV Reporter (port `rade_reporter.c` of deskHPSDR;
   needs a locator field; runs on the server).
3. **RADE phase 4 / Pi server**: aarch64 build with NEON, RNNoise model and
   RADE libraries on the Pi, CPU measurement on a Pi 4 (Pi 5 fallback).
4. **SDRplay RSP1A clone** (1DF7:2500): working with SoapyMiri, merged (section
   4.12). Still to watch: long runs (lost samples on USB2 reported
   upstream). For the Pi: build libmirisdr-5 + SoapyMiri there and
   blacklist the kernel modules `msi2500` / `msi001`.
5. Optional: panadapter "-80 dBm/H" label overlap; offer the upstream bug
   fixes of 4.6 to DL1YCF; ESP32 bridge on hardware.
6. Ideas from the user: full TS-570D coverage — the user proposes further
   CAT functions as they are found (check each on the radio first).
