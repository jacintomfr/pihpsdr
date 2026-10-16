# piHPSDR-IF — work log and state (CU2ED)

Detailed record of what has been built in this fork, why, how it was
verified, and what is still open. Kept up to date with the work; the commit
messages hold the per-change detail (hashes below are on `cu2ed` unless
marked `windows-port`). RADE has its own document: `docs/RADE-IFPAN.md`.
User-facing documentation: `docs/README-IFPAN.md` (index),
`docs/IFPAN-USER-GUIDE.md`, `docs/IFPAN-INSTALL-LINUX.md`,
`docs/IFPAN-INSTALL-WINDOWS.md`; helper scripts in `ifpan-tools/`.

Last update: 2026-10-15 (SSTV receive on the server; earlier: Pi 5 server,
sliding display, documentation, public fork).

## 1. What piHPSDR-IF is

A build of piHPSDR (`make IFPAN=ON`, binary `pihpsdr-if`, name in
`src/appname.h`, settings in `~/.config/pihpsdr-if` / `%LOCALAPPDATA%\piHPSDR-IF`)
used as a panadapter and remote front end for a **Kenwood TS-570D**:

* An SDR (first a PlutoSDR, then an RTL-SDR R820T with TCXO, now an
  **MSi2500 / SDRplay RSP1A clone** via SoapyMiri) receives the
  TS-570D's **1st IF (73.05 MHz)**; the display is centred on the Kenwood's
  VFO, read by CAT. The SDR never transmits.
* The Kenwood is controlled by **CAT** (frequency, mode, PTT, preamp,
  attenuator, antenna tuner, output power, meters).
* **TX audio**: the WDSP TX chain processes the microphone (local, or the
  remote client's) and plays it on a sound card wired through an isolation
  transformer into the TS-570D **ACC2** input.
* **Client / server**: the server (since 2026-10-11 a **Raspberry Pi 5**
  next to the radio, section 4.13) owns the radio; the client (Windows /
  Linux) is the operator's station — a mirror of everything, with
  microphone and speaker.
* **RADE V1** digital voice runs on the server (`docs/RADE-IFPAN.md`).

Repositories / branches:

| Where | Branch | Purpose |
|---|---|---|
| `Documents/pihpsdr-cu2ed` | `cu2ed` | main line (Linux + shared code) |
| `Documents/pihpsdr-cu2ed-win` (worktree of the same repo) | `windows-port` | Windows build + MSI; `cu2ed` is merged into it after every change |
| bookmarks | `stable-3.1.118/133/165`, `stable-win-3.1.118/133/165` | known-good states of 2026-10-11 (MSI versions) |
| work branches | `sdrplay`, `windows-sdrplay` | SDRplay RSP1A-clone work (merged into the main line) |
| GitHub | all of the above | public fork **https://github.com/jacintomfr/pihpsdr** (remote `origin`, pushed 2026-10-12). Remote `upstream` = dl1ycf/pihpsdr, fetch only: its push URL is disabled on purpose — nothing goes to the author without the user's explicit authorization. |

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
* **GitHub releases** (MSI downloads): `gh release create ifpan-3.1.N <msi>
  --repo jacintomfr/pihpsdr --target <full SHA of the windows-port commit the
  MSI was built from> --prerelease`. The tag name must **not** start with
  `v` (the version comes from `git describe --match 'v[0-9]*.[0-9]*'`).
  First one: `ifpan-3.1.188` (2026-10-12, commit 276da965, plain x86-64
  build for a test from outside home through Tailscale).
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
* 2026-10-13: TX Drive (TS PWR) limited to 50 W while RADE TX is armed
  (`RADE_DRIVE_MAX` in `rade_glue.h`): capped in `radio_set_drive`, brought
  down when RADE TX is armed (`rade_glue_set_tx_armed`), and a TS PWR knob
  turned above it is set back (`ifpan_power_apply`, server). Reason: full-duty
  signal, PA heat and IMD (ARRL TS-570D report: IMD3 ~ -30 dB PEP, IMD5
  ~ -45 dB at 100 W). Built on the Pi; to be tested.
* 2026-10-13: leaving RADE restores where the operator was: snapshot of
  VFO A band, band stack, frequency, CTUN, mode, filter, deviation, RX volume
  and TX Drive when RADE goes on (snapshot_take, rx or TX armed, from off),
  restored when RADE goes off (snapshot_restore, Kenwood too via
  ifpan_band_changed), also if the engine fails to start; persisted as
  rade_prev.* for a restart with RADE on. Before, only the mode came back,
  and only if it was still DIGU/DIGL. Built on the Pi; to be tested.
* 2026-10-13: RADE menu = RADE (user's choice): opening it enters RADE
  (RX on, TX armed per rade_tx_pref, DIGU/DIGL, apply_fit_filter at once),
  its Close button / window X leaves it (snapshot_restore); a destroy by
  another menu keeps RADE on. No more RADE on at startup (rade_enabled not
  used); a program end in RADE is restored at the next start
  (startup_restore, waits for CAT). Before, RADE stayed on with the menu
  closed, the RADE TX chain (no compressor, 700 - 2300 Hz TX filter) could
  stay forced in SSB, and the RADE filter was only set by Fit Filter.
  **Tested OK by the user on the Pi 5 (2026-10-13)**; Windows client
  3.1.267 installed, not tested yet.
* 2026-10-13: Quick Tune inside the span did nothing (14.210 -> 20m 14.236:
  only the Kenwood moved, the CTUN receiver stayed as it was still in the
  span; from 40m it worked). rade_tune() now sets VFO frequency, CTUN
  frequency and band like a band change (vfo_vfos_changed) before
  ifpan_band_changed. Entering RADE goes straight to the band's RADE
  frequency (Quick Tune list, one per band) if the band has one.
  Tested OK on the Pi.

### Network packet loss readout (2026-10-13)
* The server puts a sequence number in the RX audio packets (header.s2,
  marked by header.b2 = RXAUDIO_SEQ_MARK 0xA5, PCM and Opus); the client
  counts the gaps of RX1 over 5 s windows (client_rxaudio_seq) and shows
  `ms  x.x%` on the status line (fixed slot "999 ms  99.9%"; the word
  "loss" was dropped so TS MUTE still fits before the right block,
  checked in a Windows capture, TS MUTE moved 0.8 x size1 after the slot).
  First reading on the home LAN: 0.2 - 1.1 % (the old audio cuts).

### ACC2 PTT on pin 9 by USB-serial RTS (2026-10-13)
* FTDI FT232R on the Pi keys PKS (ACC2 pin 9) from RTS, through piHPSDR's
  serial PTT (Menu -> CAT, row PTT, by-id port name, Enable; on the server).
* TUNE always gave RF, MOX none: the serial PTT thread polls every 100 ms,
  so RTS came after the CAT TX; and the TS-570 then keeps the microphone
  for the whole over. Proven with a recording of the H390 monitor: a
  whistle at the TUNE tone level (-3 dBFS rms, 1.2 kHz) gave no RF.
* Fix 461d683f: serial_ptt_now() sets RTS from the CAT thread, 50 ms before
  TX; and drops it right after RX;. Diagnostics added: per-over TX
  audio levels (mic / to ACC2) and RTS / CTS changes in the log.
  **Confirmed by the user: SSB through ACC2 works every time.**
* Also fixed: the TX chain values RADE forces were saved when the program
  ended with RADE TX armed (TX filter 700 - 2300, compressor / CFC off) and
  stayed after restart; now kept as rade_txs.* and put back (9d3196ea).

### PTT safety (2026-10-14)
* Incident: the Pi booted with both FTDI adapters plugged in; the CAT bridge
  script took \`ls /dev/serial/by-id/*FTDI* | head -1\` = the FT232R PTT
  adapter (sorts first), opening it raised RTS: the TS-570 stayed in TX and
  CAT had no radio. Stopped by killing socat + pihpsdr-if (close drops RTS).
* Fixes: bridge pinned to the CAT adapter by serial number (FTCSEGX1)
  (/usr/local/bin/ts570-cat-bridge.sh, ifpan-tools/linux-pi, install doc);
  ptt_server keeps RTS at the TX state every 100 ms poll and logs
  \`WARNING: RTS was ON outside piHPSDR\`; **TX time-out** (Radio menu,
  minutes, default 3, 0 off, persisted ifpan.tx_timeout, server-enforced,
  mirrored to the client in IFPAN_DATA.tx_timeout / IFPAN_REMOTE_TX_TIMEOUT
  - client and server must be the same build).
* Remaining risk: a Pi that hangs with RTS on keeps the TS in TX (only a
  hardware timer on the pin 9 circuit would cover it); at program start the
  PTT port open raises RTS for a few ms.

### Split / VFO B / CTCSS by CAT, repeater memories (2026-10-14)
* ts_sync() in the CAT thread: piHPSDR split <-> FT, VFO B <-> FB,
  CTCSS <-> TO / TN (TS-570 manual: TN01..38 = ctcss_frequencies[] order,
  39 = 1750 Hz; "FR always places simplex": split on = FR1 MDx FR0 FT1).
  Read after connect + every CAT_MODE_EVERY polls; radio changes taken
  over (ts_sync_apply, also pushed to the client), piHPSDR changes sent;
  not while TX / tuning / pending. 50 ms between commands.
* store.c (IFPAN): normal memories also store VFO B; recall restores VFO B
  + split when stored with split, and switches split off otherwise.
  client_server.c send_memory_data sent VFO A as alt_frequency (upstream
  bug, fixed). CMD_RCL also sends radio data + VFO B to the client.
* vfo.c: with split the TS readout shows "TX <VFO B> <mode>".
* Upstream-shared files touched: store.c, client_server.c, server_thread.c
  (check when merging new piHPSDR versions). Built on the Pi, not yet
  tested on the radio.

### 10 m repeater test (2026-10-14)
* S. Miguel 10 m repeater: RX 29.630 FM, TX 29.530 (-100 kHz), 88.5 Hz.
  Split + VFO B FM + subtone ON confirmed in the log (11:05:08), a 12 s
  voice over at 11:05:33 went out on 29.530 with tone and full deviation:
  no answer -> repeater most likely down (or changed tone / shift).
* Fixes on the way: tone sync only after piHPSDR's radio is up (a default
  TN01 had overwritten the TS's 08), subtone in FM only, VFO B forced to
  VFO A's mode in split (TO1 was refused otherwise), a refused set is sent
  again instead of being taken as "changed on the radio" (split had been
  switched off that way), "?;" counted as an answer (no false "TS not
  answering"), lone "?;" not an alert, RTS follows the CAT command.
* Pi mic (Griffin iMic, Mic capture) was at 15/15 = +31 dB, peaks at
  0 dBFS: set to 9 = +25 dB (alsactl store); piHPSDR Mic slider 0..6 dB
  is enough for full FM deviation (20 dB drove the WDSP ALC hard: hiss in
  pauses). Compressor / EQ / CFC off for FM.
* Run script keeps the previous log as ~/pihpsdr-if.log.1.

### Server leaves full screen while a client is connected (2026-10-14)
* Not a bug: piHPSDR's server_thread.c switches the server display to the
  "custom" size of the client's screen while a client is connected (the
  server computes the spectrum / waterfall pixels at the client's width),
  so the Pi's 1024x600 window leaves full screen; it comes back when the
  client disconnects. Decision (user): leave it as in piHPSDR (option 1);
  a separate spectrum width from the window width was judged too invasive.

### TS-570 refuses key functions while its MENU is open (2026-10-14)
* Toolbar PRE / ATT "stopped working": the log showed PA1; / RA01; sent and
  the read-back unchanged. Direct test (pihpsdr-if stopped, socat alone):
  reads all answer (PA0, RA00, MD2, NB0, LK0, FR0, IF ...), sets of
  FA / AG / PC / AC are accepted, but **MD, NB, PA and RA sets answer "?;"**
  (also PA0 / RA00 = the current state). Cause: the user had left the TS-570
  inside its MENU (display dimmer); the radio refuses key functions then.
  Leave the menu (MENU / CLR) and they work again (they had worked before).
* No CAT command found to leave the menu: MN; and EX reads (EX001;, EX01;,
  EX00100;) answer "?;". **EX0010000; got no answer - it may have been taken
  as a SET of menu 01**: check menu 01 on the radio. Kenwood's ARCP-570 does
  change menu items, so the EX syntax exists; not probed further.
* EX format confirmed from the manual (user's PDF, command table page):
  set `EX` + P1 menu (3 digits) + P2 selection (4 digits) + `;`, read `EX;`
  answers the menu on display. So the probe EX0010000; did SET menu 01
  (beep) to OFF; put back to level 5 with EX0010005; (accepted, 2026-10-14).
  EX; answered EX0000000; = the radio still in menu 00 (display brightness
  OFF), MD2; still "?;". No CAT command leaves the menu (PS0/PS1 power
  cycle not tried: not sure PS1 wakes the TS-570, user away). Guide 5.1
  has the EX format and the useful menu numbers.
* Code kept: PRE / ATT presses, the command sent and the read-back (100 ms
  later) are now logged. Not done (user: record only): an on-screen hint
  when the TS refuses a set with "?;".

### First RADE DX test (2026-10-14)
* TS-570D in RADE (V1) through ACC2 + serial PTT, about 45 W (under the 50 W
  RADE limit), 20 m 14.236 MHz: received ~1500 km away on the SDRPT WebSDR
  (Santarém, Portugal) and decoded with the RADE-PT browser page: SYNC,
  SNR -2 dB, offset +0.6 Hz, 8.6 s of speech. Clean block on the waterfall,
  no splatter beside it.

### RADE V2 test branches (2026-10-13)
* Separate branches, the normal builds stay RADE V1: `pihpsdr-if-rade2`
  (this repo, commit 9402e6b2, docs/RADE-V2-TEST.md there) and
  `deskhpsdr-rade2` (jacintomfr/deskhpsdr, for a colleague's HL2 on Ubuntu
  24.04, also RADE on Linux). Same rade_c commit c394514 on both sides
  (V2 waveform not stable upstream; on-air use not recommended by the
  authors; tests only between the two stations). V2: no EOO callsign.
* Pi 5: separate install `~/pihpsdr-if-rade2`, desktop shortcut
  "piHPSDR-IF RADE V2 (test)"; only one of the two may run at a time.
  Loopback check (tx -> rx) OK for V1 and V2.

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
* SDR LNA off (Radio menu, `ifpan.sdr_lna_off`, also from a client;
  protocol 0x013100F5): with the strong IF buffer signal the MSi001
  overloaded above about RF 80, where libmirisdr's overall gain switches
  the +24 dB LNA on (from 43 dB; mixer from 19 dB, baseband 0..59 dB). The
  option switches the Soapy gain element "LNA" off after every RF gain
  setting; the RF slider works on the remaining stages. (piHPSDR's own
  per-stage "RX1 Gains" in the Radio menu are neither saved nor usable
  from a client.)

* PTT stall: the MSi2500 clone stops streaming (read timeouts) at the
  Kenwood's PTT, even in USB without modulation; with the IF cable unplugged
  100 W SSB gives no trouble, so it is an electrical transient on the IF
  cable (relays / TX-RX supply switching, ground loop TS <-> PC; the RTL-SDR
  did not mind). The automatic reopen brought it back after ~4 s. Now a
  stall that starts while the Kenwood transmits (or within 3 s after) is
  only noted, and the SDR is reopened at once when the Kenwood is back in
  RX. Hardware remedies suggested to the user: ferrite on the IF coax, DC
  block, an RF isolation transformer (galvanic isolation) on the IF line,
  and checking how the IF buffer is powered (the seller's recommended
  supply; the user will review the schematic).
  Narrowed down on 2026-10-11 (TS in USB without modulation, i.e. no RF;
  IF cable connected): the stall happens with the **microphone PTT** (3 of
  3, recovered ~2 s after RX by the fast reopen) but **not with PTT by CAT**
  (0 of 3 with piHPSDR-IF's MOX, 0 of 8 with a test program streaming
  through 0.5-3 s TX). Shorter cables, ferrites at both ends and a 10 dB
  attenuator did not help. So the transient comes through the microphone
  path (mic PTT line / mic preamp supply / mic cable ground), not from the
  TS's TX/RX switching. Remote operation (PTT always by CAT) is not
  affected. To check: microphone type (powered desk mic?), what else the
  mic cable is connected to, PTT with another mic / a bare footswitch on
  the mic PTT line, ferrite on the mic cable. Also seen during the tests:
  the FTDI USB-RS232 CAT adapter re-enumerated (19:50); restarting
  ts570-cat.service brought CAT back.
  **Corrected the same evening** (test program, PTT by CAT, AM carrier into
  the dummy load, IF cable connected): at **5 W no stall** (9 of 9 fine:
  streaming, stream stopped over the TX, gain 0), at **25 W a stall every
  time** (9 of 9), even with the stream stopped or the gain at 0. libusb
  then reports `failed to use alternate setting ... -1` and `submit
  transfer ... -5` (I/O error, device not found): the MSi2500 is knocked
  off / reset on the USB bus, not just its streaming engine. The trigger is
  the transmitted signal itself: the TS-570 builds its TX signal in the
  same IF chain, so while transmitting the IF buffer feeds the clone a far
  stronger signal than in RX; through the clone's input protection this
  resets its USB chip (the RTL-SDR tolerated it). The microphone was only
  a way of making RF (room noise in USB); SSB without modulation and 5 W
  AM are fine. Nothing in software can prevent it (only the fast reopen,
  ~2 s after RX). Remedies: cut the IF to the SDR while transmitting (RF
  relay or input to ground, switched by the TS's TX / REMOTE line), a TX
  mute input on the IF buffer if it has one, or a diode limiter at the
  clone input; heavy attenuation (30-40 dB) would spoil reception.
  **Refined again:** at 5 W AM the AC level the clone receives during TX is
  the same as in RX (about -67 dBFS, noise; DC removed), so the TX signal
  does not reach the SDR (the buffer's TX mute works, or the IF tap does
  not carry it). The 25 W stall is therefore most likely the TS's supply
  current shifting its ground against the PC's: current through the IF
  coax shield, through the clone, to the USB ground (ferrites do not stop
  DC / low-frequency current; an RF relay must open the shield too).
  Remedies: RF isolation transformer or a DC block in both centre and
  shield on the IF line, a heavy ground strap TS <-> PC, check where the
  buffer takes its supply / negative. Quick check: the server on a laptop
  on battery. (The clone has its own input limiter; the buffer has a TX
  mute.)

* IF offset per Kenwood mode (Radio menu "IF offset (Hz), TS <mode>",
  server only, `ifpan.mode_if_offset.N`): the TS-570D moves its carrier
  inside the 73.05 MHz IF with the mode (a station at 7.153 LSB on the TS
  was at 7.156 here while USB was right; switching LSB -> USB moved the
  signals to the left). The offset of the Kenwood's current mode is added
  to the IF and the SDR is retuned on every Kenwood mode change; the user
  sets each mode once. Confirmed by the user (MSI 3.1.151): LSB = -3000 Hz
  puts the 7.153 station on 7.153; USB 0; CW / CW-R / AM / FM not checked
  yet. TS MUTE confirmed working from the toolbar.

* Toolbar action **TS MUTE** (`IFPAN_TS_MUTE`): the Kenwood's speaker (AF
  gain, CAT `AGnnn;`, checked on the radio: AG080 / AG000 read back) to 0
  and back to its saved level, to compare its audio with piHPSDR-IF's
  without touching VOL. Turning VOL up while muted ends the mute (AF gain
  polled while muted); the level is put back on exit; "TS MUTE" in red on
  the status line above the dial; works from a client (protocol
  0x013100F6).

* Radio menu made compact (3.1.155-3.1.159): the IF panadapter options were
  one per row in the right-hand column (stretched by the long sound card /
  CAT combos) and the SDR gains came only below them. Now the Kenwood /
  IF panadapter options sit two per row under the general settings, the
  SDR gains under the right-hand column's SDR settings, and the sound card
  combo is ellipsized (width-chars / max-width-chars 22). Window 2634x1642
  -> 2322x1272 px (200 % scaling) before the ellipsis, then narrower still
  (user's capture); no overlaps or cut-offs.
* Mouse-wheel tuning jumped back and forth (3.1.161): the TS-570 drops FA
  commands that arrive while it is busy, and after the 400 ms hold the
  display followed its stale frequency. Now the target is sent again (up
  to 3 times) before the display follows the Kenwood. A rare one-step
  flick back remained (a Kenwood reading taken before the newest target was
  sent): with "Clicks tune the TS" readings are no longer applied while a
  change is not yet sent and for 0.5 s after the last one (3.1.165) -
  confirmed "perfect" by the user.

### 4.13 Raspberry Pi 5 server (SDR-IQ, 192.168.0.107) (2026-10-11)

Purpose: test whether the MSi2500 PTT stall (ground loop through the IF coax
shield, see 4.12) also happens with the Pi 5 as host, and measure RADE there.

- Host: Pi 5 4 GB, Debian 13 trixie aarch64, labwc (Wayland), PipeWire/pulse.
  SSH as `pi` (key `hpsdr_rpi5`); temporary NOPASSWD sudo in
  `/etc/sudoers.d/010-pi-claude-setup` (remove when the setup is finished).
- Packages: libsoapysdr-dev soapysdr-tools libspeexdsp-dev libusb-1.0-0-dev
  socat. Kernel DVB drivers blacklisted in
  `/etc/modprobe.d/blacklist-msi2500.conf` (msi2500, msi001, sdr_msi3101);
  udev `/etc/udev/rules.d/99-msi2500.rules` (1df7:2500 MODE 0666).
- **Trap:** `soapysdr0.8-module-all` pulls `soapysdr0.8-module-mirisdr`
  (driver "miri", old libmirisdr-4). It enumerates the clone first; piHPSDR
  then picks driver=miri and 768 kS/s. Removed that package; only
  `soapyMiri` remains (DriverKey MIRI, 1.536 MS/s, ISOC).
- libmirisdr-5 + SoapyMiri (ericek111, 1e0a125) built in `~/deps`, installed
  to /usr/local (lib) and /usr (Soapy module); `soapymiri-transfer-mode.patch`
  applied (it needed `sed 's/\r$//'`: copied from Windows with CRLF).
- RADE libs: `RADE_C_DIR=~/rade_c ./build-rade.sh` (the Opus model download,
  ~180 MB at ~200 KB/s, is most of the time).
- Source: `git archive cu2ed` extracted to `~/pihpsdr-if`, CRLF stripped
  (rnnoise/model_version with CRLF broke the rnnoise model curl URL).
  `make.config.pihpsdr`: GPIO=OFF MIDI=ON USBOZY=OFF SOAPYSDR=ON IFPAN=ON
  PORTFORWARD=ON TCI=ON AUDIO=PULSE. Binary `~/pihpsdr-if/pihpsdr-if`,
  launcher `run-pihpsdr-if.sh` + `~/.local/share/applications/pihpsdr-if.desktop`,
  log `~/pihpsdr-if.log`. Config: Windows `soapyMiri.props` copied, audio
  devices cleared (choose the Pi devices in the menus); the Pi default is kept
  as `soapyMiri.props.pi-default`.
  The copied props also carried "Server runs protocol only if client is
  connected" (radio.server_stops_protocol=1): with no client the Pi showed a
  black spectrum. Switched off in the Server menu (2026-10-12).
- CAT: service `ts570-cat.service` (`/usr/local/bin/ts570-cat-bridge.sh`):
  socat 127.0.0.1:7373 <-> first `/dev/serial/by-id/*FTDI*`, 57600 raw. It
  waits until the FTDI adapter is plugged into the Pi.
- Measured (RX only, no RADE, 1.536 MS/s): ~93 % of one core in total
  (~23 % of the 4 cores), SoC 60 C.
- USB: behind the external USB 2.0 hub (shared with the FTDI and the Arduino
  MIDI/audio) libmirisdr reported ~3.5 "504 samples lost" lines/s; on a
  direct (blue) Pi port ~8 per minute. Keep the MSI on a direct port.
- Desktop shortcut `~/Desktop/pihpsdr-if.desktop` at x=776 y=142 (under
  deskHPSDR); a matching box was drawn into `wall_boxes.png` (original kept
  as `wall_boxes.png.before-pihpsdr-if`).
- **Result:** SSB 100 W from the microphone with the Pi as host: no stall
  (with the Windows PC as host, AM 25 W into the dummy load always stalled).
  The ground loop goes through the PC; on the Pi the problem is gone.
- **Audio cuts (~0.5 s) with server on the Pi, client on Windows:** network.
  UDP echo test PC->Pi->PC, 100 pkt/s x 1400 bytes (scratchpad udptest.py,
  Pi ~/udpecho.py): 0.5-0.9 % lost, single packets, RTT 0.8 ms. In a 2 min
  run the Pi counted 54 new rx_frame_check_sequence_errors (+9 symbol
  errors) for 56 lost packets: frames arrive corrupted at the Pi (physical
  layer: cable / switch port / RF). The PC's Realtek GbE negotiated only
  100 Mbps (Gigabit needs all 4 pairs: suspect cable). Same symptom the
  user had once with the HL2 behind the switch (fixed by a direct link).
  The RX audio is UDP without sequence numbers, so each lost packet drains
  the client's output buffer; under 512 samples it inserts ~170 ms of
  silence. Diagnostics (252743d1 + fix) log server send gaps, client
  arrival gaps and output buffer dry/overrun with timestamps (MSI 3.1.177).

### 4.14 Display slides while the Kenwood follows (2026-10-12)

User report: with "Clicks tune the TS", turning the wheel moved the filter
ahead; when the wheel stopped it jumped back to the centre as the Kenwood
followed (CAT answer, then `cat_apply` re-centres). Also, at 96k the new
part of the band "came up from the bottom" after the re-centre at the span
edge: the TS-570 mutes its receiver while its PLL relocks, plus the display
averaging (lower the Display menu averaging; 384k + zoom avoids most
re-centres).

Change (display only; audio and tuning unchanged):
- `ifpan_display_shift(rx)`: after a tuning step made here (the receiver /
  CTUN frequency changed), with "Clicks tune the TS" on, for at most 2 s,
  returns ctun_frequency - frequency (the part the Kenwood has not followed
  yet); 0 otherwise. Turning the Kenwood dial with "Follow Kenwood" off
  does not change the CTUN frequency, so its offset is not touched. Works
  in the client too (no CAT state needed).
- `ifpan_shifted_pixels()`: the pixel samples moved by shift * cAp pixels,
  -200 dB in the empty strip (slot 0 panadapter, 1 waterfall).
- `rx_panadapter.c`: centre frequency += shift, filter/cursor offset -=
  shift (filter stays in the centre), shifted samples; the dashed Kenwood
  line is drawn where the Kenwood really is (cBp - shift).
- `waterfall.c`: frequency += shift (its own horizontal rotation then
  slides the old rows at once), new rows from the shifted samples. When the
  Kenwood confirms, frequency + 0 = the same display frequency: no jump.
- Known limit: a click on the panadapter during the slide (< 1 s) maps
  with the shifted axis.
- **Confirmed by the user (2026-10-12, Pi 5 build, 96k):** "does exactly
  what I want". Tuning now behaves like a normal radio: the filter stays in
  the centre, the spectrum and waterfall slide, the Kenwood follows.

- **Follow-up (2026-10-12 evening, MIDI VFO at 96k):** the old effect came
  back when turning the MIDI VFO fast. Cause: piHPSDR's CTUN keeps the
  receiver inside the span (`vfo_id_step()` / `vfo_id_move()` refuse a step
  whose passband would cross frequency +- sample_rate / 2). At 96k (+-48 kHz)
  a fast knob reaches the edge before the Kenwood has followed; the steps
  were refused, the slide stopped (no CTUN change for 2 s) and the filter
  showed at the edge until the Kenwood caught up. In the morning test the
  knob was turned slowly. Fix: with "Clicks tune the TS" the edge check is
  skipped (`#ifdef IFPAN`, only when `ifpan_cat_write`): the receiver may run
  ahead, the display slides, the Kenwood follows and the display re-centres.
  (While the receiver is beyond the span its audio is briefly silent.)
- **Real cause found the same evening:** the vfo.c change alone did not
  help (user screenshot: right ~36 kHz of the span empty/low for seconds,
  TS = VFO). While the knob turns continuously, cat_apply ignored every
  Kenwood reading (the 0.5 s wheel hold) and the CAT thread ignored readings
  during each 400 ms set hold, so the display centre (vfo.frequency) froze at
  the old Kenwood frequency while the Kenwood - and the IF - kept moving.
  The slide was computed from that stale centre (data shifted too far, empty
  strip), the receiver offset (audio) was off by the same amount, and the
  final re-centre shifted the WDSP display average by tens of kHz at once
  (edge values smeared over the gap until the average recovered). Fix
  (`centre_only()` / `centre_apply()`): during tuning every Kenwood reading
  moves only the display centre (and the display average, in small steps),
  never the CTUN receiver; readings taken during the set hold are posted for
  this too. The receiver stays on the target, its offset is recomputed, and
  the slide is drawn from where the Kenwood really is. The SDR is not
  retuned (IF unchanged, cached in soapy_protocol.c).
- **And the user found the knob speed itself (2026-10-12, late):** the MIDI
  VFO is an optical encoder with 600 pulses per turn; at a 100 Hz step and
  "VFO Encoder Divisor" 1 one turn is 60 kHz - more than the TS-570 can
  follow by CAT, so the slide went in jumps. With the divisor at 60 the knob
  is slower but fluid. The morning build (1667b45b, rebuilt on the Pi as
  ~/pihpsdr-if-manha for an A/B test) behaved exactly like the evening one:
  the difference was how fast the knob was turned. Side problem found: since
  ba55e0d9 (2026-10-10, before any MIDI VFO) the CAT poll interval was
  ifpan.poll_ms x the divisor, so a divisor of 60 meant one Kenwood reading
  every 3 s. Now the poll is always ifpan.poll_ms (50 ms) and the divisor
  only sets the encoder speed.
- Tried and **reverted** (2026-10-13, d604b1d4 / 5728cb01): a "sliding
  photograph" (last picture taken while the Kenwood was settled, moved while
  it changes frequency). With the MIDI VFO turning steadily the Kenwood is
  almost always moving, so the picture stayed frozen for the whole turn; the
  user wants live signals sliding, as with the Hermes-Lite 2.

**Tuning from a MIDI VFO instead of the Kenwood dial (the remote setup).**
The user tunes the Pi 5 server with a MIDI VFO knob (the Arduino Due
controller with the 600 PPR optical encoder, project
`jacintomfr/hpsdr-midi-optical`, USB MIDI on the Pi). The TS dial is not
touched any more, which is what a remote station needs: the operator turns
a knob at the piHPSDR-IF end, piHPSDR-IF tunes the Kenwood by CAT. Settings
for this way of working (Radio menu, "Kenwood / IF panadapter"):
- "Clicks tune the TS" **on**: every tuning step (MIDI VFO, mouse wheel,
  clicks) is sent to the Kenwood; with 4.14 the display slides at once.
- "Follow Kenwood" on (the receiver stays on the Kenwood frequency).
- CTUN on (piHPSDR default in IFPAN).
- Span: 96k gives the most detail around the station and is fine now that
  the display slides (the re-centres at the span edge are smooth). 384k +
  zoom remains the choice for watching a whole band.
- With "Clicks tune the TS" off the spectrum stays still and only the
  receiver moves inside the span; "TS here" (toolbar) then brings the
  Kenwood to the receiver before transmitting.

## 5. Open items (updated 2026-10-12)

0b. **CONFIRMED 2026-10-13 01:10: SSB at 100 W through ACC2.** Microphone -> piHPSDR-IF WDSP TX chain -> Logitech H390 (100 %) -> 600 ohm transformer -> ACC2 pin 11 (shield pin 12), TS keyed on PKS (pin 9 grounded by hand for now), menu 33 = 2, PROC off. TUNE at the programmed 10 W also confirmed. No USB hub reset during the 100 W test. Remaining: automatic PKS keying (GPIO17 + optocoupler, option "PTT via ACC2"), then TwoTone / ALC with speech / RADE TX.
0c. **Fluid tuning, measured (2026-10-13, ifpan-tunelog):** mouse-wheel steps reached the CAT thread every ~50 ms, but the CAT cycle (read the Kenwood, send the next FA) took 120-150 ms: after each reading taken while a set was on its way the thread slept a fixed ifpan.poll_ms (50 ms). The Kenwood (and the IF) moved only 7-8 times a second in jumps of up to 2 kHz (~20 px at 96k). Now hold_wait() returns as soon as a newer target is waiting (10 ms checks). To verify with the user. Measured again: still a 124 ms median between sets - the read-back of the Kenwood after every set is the slow part (TS-570 answers in tens of ms). Now "trust the set": every FA sent moves the display centre at once (centre_apply), and while targets keep arriving (TRUST_SET_US = 150 ms after the last set) the Kenwood is not read back; one reading after the knob pauses confirms it (the resend logic still corrects dropped commands). Then 25 ms steps (ae00a204): ~38 sets/s, TS-570 dropped 2 of 238 and missed some readings. Screen capture (grim, PNG ~10 fps, ~/capseq.sh) + log showed the visible stalls: a reading right after a burst of sets sometimes gets no answer (0.5 s socket timeout); one missed answer marked the Kenwood "not answering" (TS greyed, ifpan_cat_connected = 0), paused another 0.5 s, and ui_watch dropped the steps made meanwhile (~1 s stall, then the filter went back). Now: two missed answers in a row are needed, no pause while steps are waiting, and steps made while CAT is down are kept and sent when it answers again.
0d. **Fluid tuning, result (2026-10-13, mouse, 96k, 100 Hz):** 25 ms steps (~38 sets/s) made the TS-570 drop commands and go silent for up to 3 s, so back to 50 ms (0cc2280d). Final capture at 50 ms: ~20 sets/s, **0 missed answers, 0 dropped commands (112 checks), no CAT reconnect**, TS line within 1-2 kHz of the filter while turning fast. The screen capture then showed the waterfall black while tuning: piHPSDR skips the new row whenever the frequency changed in that frame (old IQ artefacts), and the centre now changes ~20 times a second; in IFPAN a row is drawn on every frame (waterfall.c, #ifdef IFPAN).
0e. **Click near the span edge (large jump), 2026-10-13:** the new part of the display stayed empty ~0.5 s and then rose from the bottom (worse the further from the centre). ShiftDisplayAverage filled the freed pixels with the old edge value, which lies in the filter roll-off far below the noise. Now they get a noise-floor estimate of the kept part (25th percentile of 31 inner samples, wdsp/analyzer.c floor_estimate; only used by IFPAN). Note: the top-level make does not rebuild wdsp/ after a change there - run make -C wdsp (the Windows build-exes.sh does make clean). Still seen: a bright horizontal line in the waterfall at the moment of a large jump.
0f. **Clicks / large jumps, final state (2026-10-13):** freed pixels get the median noise level and, after a jump > 5 % of the span, the 4 % next to them as well (no more empty strip rising from the bottom, no gap). What remains is a 0.1-0.2 s dip / micro-step right after the click: the TS-570 itself mutes its receiver while the PLL locks (the whole IF drops, captured frame by frame). Tried hiding it by holding the last picture 300 ms after an isolated large jump (5d6d51ce): the user found it worse, reverted (4771e650). Accepted as is.
0g. **Side effect while turning continuously (MIDI VFO simulated with the mouse), 2026-10-13:** the side being tuned to broke into a saw-tooth / islands. Tried moving the display centre 150 ms after each set (e8083258): worse (TS line 10-15 kHz behind, saw-tooth still there), reverted (47699380). Real cause: the WDSP display average holds frames taken with the Kenwood at slightly different frequencies; each frame's edge roll-off, moved by the small shifts, stacked up. Fix 6ac0abbe: the 4 % next to the freed pixels get the noise level on every shift, not only after large jumps. Capture: clean leading edge, no saw-tooth; user: better. MIDI VFO itself still to retest at home.
0a. **USB hub resets while transmitting** (2026-10-13 01:01-01:07, TUNE at 10 W into the dummy load with the new ACC2 cable): the hub on Pi port 1-1 (FTDI CAT adapter + Logitech H390) disconnected and came back three times; PipeWire then moved the ACC2 stream to the iMic (no TX audio) and the CAT thread reconnected thousands of times a second to the socat bridge (log 3.4 MB). Fixed in software: at most one CAT connection attempt per second. To do in hardware: H390 and FTDI straight on Pi ports or a powered hub, ferrites on the USB and ACC2 cables, keep the H390 side of the transformer floating, short pin-9 wire.
0. **ACC2 PTT via PKS (pin 9)** (2026-10-13): cable made, Logitech H390 as the ACC2 sound card (needed `systemctl --user restart wireplumber`). TX by CAT `TX;` uses the front mic - the TS-570 takes ACC2 audio only when keyed on pin 9 (verified: pin 9 to ground + 1 kHz tone at H390 100 % = RF; at 40 % nothing). Next: GPIO17 + PC817 optocoupler to pin 9/8 and a Radio-menu option "PTT via ACC2" (user guide 5.1).
1. **ACC2 cable** (user is looking for the plug; ACC2 pin-out and menus 32 / 33 / 34 from the TS-570 manual are in the user guide, section 5.1: audio into pin 11 PKD, shield pin 12; menu 33 input level 0..2, menu 32 OFF). Then test on a dummy load:
   TUNE / TwoTone power, ALC level (set by the sound card volume), CW → USB
   switch during TUNE, FM deviation, SWR protection during TUNE, and
   **RADE TX** (TS-570D PROC off, TX EQ flat, ALC low; PTT held ~0.5–2 s
   for the EOO).
2. **RADE phase 3**: FreeDV Reporter (port `rade_reporter.c` of deskHPSDR;
   needs a locator field; runs on the server).
3. **RADE phase 4 / Pi server**: the Pi 5 build with RADE is done and runs
   (4.13); still to measure the CPU while decoding RADE on the Pi.
4. **SDRplay RSP1A clone** (1DF7:2500): working with SoapyMiri on Windows and
   on the Pi 5 (4.12, 4.13). Still to watch: long runs.
5. **LAN packet loss PC <-> Pi** (4.13, client audio cuts): the user is
   replacing cables / switch port; then re-run `ifpan-tools/net/udptest.py`
   and, if cuts remain, install the diagnostic builds (MSI 3.1.177+ on the
   client, Pi binary already built) and read the `audio diag:` lines.
6. **TX chain dead after an SDR stall during MOX** (Windows PC server only:
   TX spectrum / mic / ALC empty until restart). Not seen with the Pi
   server; `mic diag:` lines are in place if it recurs.
7. IF offset per mode for CW / CW-R / AM / FM (only LSB / USB set).
8. Pi 5 housekeeping: remove the temporary sudo rule
   `/etc/sudoers.d/010-pi-claude-setup`.
9. Optional: panadapter "-80 dBm/H" label overlap; ESP32 bridge on hardware;
   the upstream bug fixes of 4.6 are candidates for DL1YCF **only if the
   user authorizes it** (nothing is sent upstream otherwise).
10. Ideas from the user: full TS-570D coverage — the user proposes further
   CAT functions as they are found (check each on the radio first).

### SSTV receive, phase 1 (2026-10-14 / 2026-10-15)
* Main menu "SSTV" (TX column, after RADE), server only (on the client it
  says so). hpsdr-rs's SSTV modem: `sstv-lib/src/sstv.rs` is hpsdr-rs's
  `src/sstv.rs` (from SDRoxide, GPL-3), built as a Rust static library
  (`sstv-lib/`, C interface in `src/sstv_lib.h`, glue `src/sstv_glue.c`,
  panel `src/sstv_menu.c`). Built in WSL for aarch64, the `.a` copied to the
  Pi's `~/pihpsdr-if/sstv-libs/lib/` (no Rust on the Pi; `make` turns
  SSTV on when the library is there). Windows build: not yet.
* Panel docked at the right of the spectrum / waterfall, which shrink like
  two receivers side by side (radio_reconfigure: sstv_panel_wanted_width /
  sstv_panel_place). hpsdr-rs layout: picture, Sync / Lvl / % bars, state,
  Decode Auto (green when on) / mode, Restart RX, Fit Filter, Auto-save
  (`~/pihpsdr-if/sstv_images/pihpsdr-if_<time>_<mode>.png`), Quick Tune.
* Decoder input: RX1 audio before RADE, the AF gain undone (independent of
  the volume slider); squelch off while SSTV decodes, put back on close.
* Opening SSTV: zoom 26 with the SSTV band (1750 Hz) centred by the pan,
  Fit Filter (Var1 1100 - 2400 Hz, mirrored for LSB/DIGL), tuning lines on
  panadapter and waterfall as in hpsdr-rs (1200 / 2300 Hz purple, 1500 Hz
  blue). Close (and Menu - Exit with the panel open, sstv_menu_shutdown)
  puts zoom, pan, filter, Var1 edges and squelch back as before SSTV.
  (An exit with the panel open used to save zoom 26 / Var1 1100 - 2400 /
  squelch off: cleaned once in soapyMiri.props on the Pi, backup
  `soapyMiri.props.bak-sstv`.)
* Decoder changes (local additions in sstv.rs, made the same in hpsdr-rs,
  commits a65ca71 and ced288a there): off-air signals did not decode at
  all. Replayed recordings + synthetic pictures with noise (WSL tool):
  - the VIS / sync detectors use a frequency smoothed ~1 ms (DET_ALPHA
    0.02) instead of the per-sample one: synthetic cases 15/80 -> 73/80,
    lock down to ~3 dB SNR (2.5 kHz) instead of ~15 dB;
  - free-run (picture picked up without VIS) needs 3 cadence matches
    (FREERUN_MIN_HITS, upstream 2);
  - lock check: the first 8 lines must show a sync pulse where the mode
    has one at least 4 times, else the lock is dropped (LockDropped event,
    nothing saved). Receiver-filtered noise false-locked Robot 36 / SC2 /
    Martin ~4 times an hour; now none in an hour.
  - a "mode guess while hunting" was tried and dropped: right only 0 - 8 %
    of the time at 0 dB, wrong more often than right, and it showed modes on
    plain noise.
* Off-air: Martin 2, Scottie 2 ("CQ SSTV") and Robot 36 decoded on 20 m.
  Pictures picked up halfway (no VIS) on weak signals can come out shifted
  sideways: the per-line re-alignment (+-12 ms) cannot pull a wrong first
  alignment back. Open item, needs a recording of such a case.
* A strong S-meter reading is not a strong SSTV signal: one "S8" session
  measured only 1 - 4 dB of in-band power above the pauses (SNR ~0 dB);
  any decoder gives a noisy picture there.
* Diagnosis: with `~/sstv-dump.on` present when SSTV opens, the decoder
  input goes to `/tmp/sstv_in.f32` (float, 48 kHz, max 5 min) for replay
  through sstv.rs offline.
* Next: SSTV picture to the Windows client (protocol) + Windows build of
  sstv-lib (x86_64-pc-windows-gnu); then TX.

### SSTV receiver = MMSSTV's (2026-10-15)
* The hpsdr-rs decoder (sstv.rs SstvRx), even with the local tweaks above,
  lost weak off-air pictures. MMSSTV's receive chain was ported to Rust:
  `sstv-lib/src/mmsstv_rx.rs` (MmRx, same events; source github.com/n5ac/
  mmsstv, LGPL; same file in hpsdr-rs). Runs at 12 kHz (48 kHz low-passed,
  /4). Stages with MMSSTV's values: wide BPF, AGC, 1080/1200/1320/1900/2100
  Hz tone detectors (energy, not per-sample frequency), sense level 1, VIS
  state machine, free-run starts (CSYNCINT), Hilbert demodulator, AFC
  (-200..+125 Hz, 15 pulses), picture rebuild (sync position, resync at
  line 16, colour), AutoSync. Auto stop off (MMSSTV default; it threw away
  a real weak picture off the air). Auto slant and FSK-ID decode ON (MMSSTV
  ships them off): slant removes the clock-error penalty, FSK-ID 5/5 down
  to 3 dB, no false ID in an hour of noise.
* Measured (test crate, synthetic pictures + receiver-filtered noise, and
  off-air recordings made on the Pi): pixel error about half of SstvRx's at
  every SNR, VIS still at 0 dB, fewer wrong-mode locks, no false lock on
  noise. Off air, same audio through both: MMSSTV 3/3 of CU2ED's own test
  pictures (2 x Robot 36, Scottie 2, ~6 dB in the 1.3 kHz filter), the
  original hpsdr-rs decoder 0/3 (not a tuning error: shifted to the exact
  frequency it still failed); Martin 2 from G7LHK readable with MMSSTV,
  half lost with the original. Weak point: a station 100 Hz off gives wrong
  colours in some modes (MMSSTV's fixed VIS / AFC windows).
* Mode box = forced start in that mode (only when the choice changes);
  SSTV always opens in Auto; the mode is no longer saved.
* Panel: the picture is copied once more when reception ends (MMSSTV
  redraws the whole picture in its last step, the window used to keep the
  earlier version); the state / station labels have a fixed width so the
  bars column no longer jumps between "(receiving)" and "(last)".
* TX FSK ID (sstv.rs SstvTx::with_fsk_id, used by hpsdr-rs): bits are now
  sent least significant first and the closing 2100 Hz guard tone added, as
  MMSSTV sends them; before, MMSSTV could not read our ID.
* Diagnosis recording (`~/sstv-dump.on`) now starts afresh at every opening
  of the panel (it used to record only once per program run).
* "Mode guess while hunting" and a hunting AFC of +-300/400 Hz were tried
  and dropped (unreliable / false locks on the 1500 Hz black level);
  MMSSTV and QSSTV do not search wider than about +-150..200 Hz either.
* Open: Robot 36 "pictures" of plain noise seen once (17:39-17:44) before
  the recordings started — not reproduced since.

### SSTV panel laid out as the hpsdr-rs kiosk (2026-10-15)
* On the 1024 x 600 screen the control strip asked for ~610 px in a 456 px
  panel, so the whole main window grew past the screen (bars, state and
  Station cut off). Now as hpsdr-rs's digital_inapp.rs: the panel takes half
  the width (as piHPSDR's two receivers side by side) and holds only the
  picture and its status; the status texts are 13 px so the column stays
  104 px (bars) at the right edge and the picture gets the rest.
* OPTIONS (toggle) at the bottom left of the waterfall (6 px in, 44 px up,
  as hpsdr-rs); opened, the controls sit just under the waterfall over the
  slider area (the user's choice: spectrum and waterfall stay in view) -
  Quick Tune (popover with the bands, as hpsdr-rs's sstv_quick_popup),
  Auto-save / Decode: Auto, mode, Restart RX, Fit Filter. Rows wrap if
  narrow. Not shown by radio_reconfigure's show_all (no_show_all).
* Close button gone from the panel (Menu -> SSTV closes it, as the kiosk's
  DIGITAL button). Mode list as a 4 x 4 grid (one column filled the screen).
* The received FSK station ID is written to the log ("SSTV station ID
  (FSK): ...").
* FSK-ID test from the user's old Windows hpsdr-rs: its ID was MSB first
  (decoded by hand from a recording: 2A C U 2 ...), unreadable by MMSSTV
  and by us; new Windows MSIs (normal + noAVX2) and kiosk 0.8.7-563 send it
  LSB first.

### RADE panel docked like SSTV, on-screen keyboard (2026-10-16)
* The RADE window sat over the main screen. rade_menu.c is now a docked
  panel (radio_reconfigure: rade_panel_wanted_width / rade_panel_place,
  half the width as SSTV; SSTV and RADE close each other): zoom 26 centred
  on 1500 Hz audio (again 300 ms after entering / Quick Tune), zoom / pan
  back on leaving and on program exit (rade_menu_shutdown). Menu -> RADE
  (and the RADE action) toggles it; no window position file any more.
* Layout measured on the 1024x600 screen: 40 px rows, check boxes as big
  squares with an X (28 px, CSS, Adwaita window-close-symbolic, also in the
  Windows MSI), their text 17 px, status / callsigns / filter text 13 px,
  options in two columns (three widened the window past the screen), EQ
  sliders behind "Adjust" and Quick Tune bands in pop-ups; ends at y ~ 440
  of 447.
* On-screen keyboard (src/osk.c, IFPAN_OBJS): as hpsdr-rs kiosk_keyboard.rs
  (layout, 42 px keys, Shift one letter, ?123 layer, Backspace repeat
  0.45 s / 70 ms, keys never take the focus); a non-modal popover on the
  focused GtkEntry (focus-in emission hook, every field of the program), the
  width of its window. On on Linux, off on Windows. Tested on the Pi with
  My callsign.
* "Dropped" in the RADE status counts RX samples thrown away while RADE
  transmits (rx ring full): not a fault.

### Touch style, Radio menu scrolls (2026-10-16)
* osk.c: one screen CSS (priority APPLICATION - 1, just below css.c) with
  the keyboard's key look for every button (dark, 1 px border, 5 px rounded
  corners, lighter pressed, orange :checked) and 24 px X check boxes
  (Adwaita window-close-symbolic). css.c's named buttons (state colours,
  menu, hide) and widgets with their own style (SSTV Auto green, keyboard,
  RADE boxes) keep theirs. Option "Touch style" in the Radio menu's
  Kenwood block, saved as ifpan.touch_style; default on on Linux, off on
  Windows. Checked by capture on the Pi.
* radio_menu.c (IFPAN only): the grid in a scrolled window, at most
  screen height - 140 px; on 1024x600 the bigger check boxes pushed the
  menu past the bottom. Where it fits nothing changes.

### Windows client crash with the side panels; client spectrum (2026-10-16)
* Client 3.1.362 crashed (APPCRASH 0xc0000374, heap corruption) when the
  RADE panel was opened again. Found with a debug build (CFLAGS="-O1 -g"
  in the windows-port checkout) under MSYS2 gdb: detected in a cairo
  malloc in rx_panadapter_update. Cause: right after rx_update_width the
  panadapter widget / waterfall pixbuf are still as wide as before until
  GTK re-allocates; rx_panadapter_update's samples[mywidth - 1] = -200
  then wrote past the new, smaller sample buffer (and the loops read past
  it). Fix (cabb1826): the drawing width is clamped to rx->pixels in
  rx_panadapter.c and waterfall.c. A piHPSDR bug too, only exposed by the
  frequent width changes of the side panels. A first guess (configure
  handlers racing the network thread) was wrong: client_spectrum runs as
  a g_idle callback in the GTK thread.
* Client spectrum froze while a side panel was open: the server sends the
  width set by CMD_SCREEN (which also sizes the server's own screen), the
  client's receiver was half that and dropped every frame. client_thread.c
  (IFPAN) now resamples to the client's width (max of the covered points)
  and scales cB / cAp / cBp; also the TX spectrum.
* Tested by the user on the debug client: RADE opened / closed repeatedly,
  no crash, spectrum moving.
* Tip: a debug client run from the checkout needs the WDSP wisdom file next
  to it (copy %LOCALAPPDATA%\piHPSDR-IF\wdspWisdom01), else it plans the
  FFTs again for minutes.

### Touch style: bigger Menu, thin headers, big Close (2026-10-16)
* osk.c touch CSS: main Menu tiles at least 40 x 90 px (#ifpan_main_menu,
  named in new_menu.c), check boxes 30 px (were 24), #close_button at least
  40 x 90 px.
* new_menu.c (IFPAN, touch style): the Menu gets the main window's width
  (gtk_window_set_default_size) and 22 px side margins (24 px from the
  screen sides). labwc did not centre the dialog (it opened right-aligned,
  near the Menu button) and an application cannot place a window under
  Wayland; a full-width window can only sit at the left edge.
* Header bars (osk.c, emission hook on "show" of a window, before its size
  is worked out): the small "X" is removed and has_subtitle set off. The
  empty subtitle line was what made every header 68 px high (css.c already
  sets min-height 0); now 34 px. The window is resized to fit (it kept the
  old height otherwise). Windows without a "Close" button of their own
  (discovery, exit, action dialog, test, TX window) get a 110 x 34 Close in
  the header, which closes the window via delete-event like the "X".
  A CSS-only bigger titlebutton showed the theme's X twice: dropped.
* sliders.c (IFPAN): the sliders grid is named ifpan_sliders; its Sqlch
  check box stays 24 px with no padding. The grid has 36 homogeneous
  columns of ~28 px and the 30 px box widened them all, pushing RF / Sqlch
  past the right side of the screen.
* All checked by capture on the Pi at 1024x600.

### Touch layout: Menu / Hide out of the corner, bigger S-meter (2026-10-16)
* The Menu / Hide buttons in the top right corner were hard to hit with a
  finger. radio.c (IFPAN, touch style with slider rows, touch_layout()):
  - MENU_W (0 in the touch layout, else MENU_WIDTH) replaces MENU_WIDTH in
    choose_vfo_layout and the VFO width; ifpan_place_buttons() (end of
    radio_reconfigure and after TX -> RX, which puts the receiver panels on
    top again) re-runs choose_vfo_layout when the layout changes and places
    the buttons; fixed_raise() re-adds them so they draw over the panels.
  - Two slider rows: Menu and Hide 90 x (SLIDERS_HEIGHT - 8) at x 36, one per
    row, in a 90 px column left of the sliders (sliders.c: sliders_x, the grid
    is created 90 px narrower); one row: Menu there, Hide in the waterfall.
  - Hidden (no slider rows): Show 100 x 40 in the bottom right corner of the
    receivers' area (24 px from its right side, 8 px from the bottom; left of
    a docked SSTV / RADE panel, clear of the SSTV OPTIONS button on the left),
    Menu 12 px left of it. The S-meter keeps its width.
* The freed 60 px first all went to the VFO bar: the meter is capped at
  2 x VFO_HEIGHT. In the touch layout the cap is 8/3 x VFO_HEIGHT: the
  dual-scale meter grew from ~164 to ~215 px.
* meter.c rxmeter_dualscale (touch style): no dBm tick scale above the bar
  (-121 / -97 / -73, the user's choice; the dBm readout stays); the S readout
  0.34 x VFO_HEIGHT, the dBm readout 0.21 x VFO_HEIGHT, sized by the height
  (the width-based sizes made the text run into the bar on the wider meter).
* osk.c: Hide / Show dark with a light outline (inset box-shadow: css.c sets
  border 0 at a higher priority), lighter on hover / press. Menu / Hide text
  bold through a provider at APPLICATION + 1: css.c's "* { font-family }"
  picks "Open Sans", which has no bold face on the Pi (only a separate
  "Open Sans Extrabold" family), so it showed light; Menu text kept black
  in :backdrop too. "Cancel" counts as a window's own Close (the Exit dialog
  got a second, header Close).
* Checked by capture on the Pi: normal and hidden state, meter, Exit dialog.
