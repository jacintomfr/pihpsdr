# piHPSDR-KX3 — briefing for a new session

You are taking over a new project, **piHPSDR-KX3**: the Elecraft **KX3** version of
**piHPSDR-IF**, a fork of piHPSDR (DL1YCF) that the user (CU2ED) and Claude built
for a **Kenwood TS-570D** in October 2026. Read this whole briefing first, then
the documents it points to, before writing any code.

---

## 0. How to work with this user (standing rules)

* **Reply in Portuguese** (European). Code, comments, commit messages and repo
  docs stay in **English** (repo docs English only unless told otherwise).
* **Never contact upstream**: the public fork is `jacintomfr/pihpsdr`; remote
  `upstream` = dl1ycf/pihpsdr is fetch-only (push URL disabled on purpose).
  No push / PR / issue / mail to DL1YCF without explicit authorization.
* **Small, revertible, surgical changes.** Every IFPAN/KX3 change lives inside
  `#ifdef` blocks; warn before widening the diff in upstream files (merge-conflict
  risk) and propose the narrower option first.
* **Read the reference implementation before copying behaviour "like piHPSDR"**
  (local clones in `Documents/`), state what you found, then code.
* **Compiling is not verifying.** For every visible change: capture the Pi screen
  (`grim`), look at it, measure, then report. Never say "OK" from a build alone.
* **TX tests on air or into the dummy load need the user's OK each time.**
* **Persist every setting** (props file) and restore it at start.
* **Fixed-width numeric readouts**, no layout jumps. Touch UI: big rounded
  buttons, big check boxes with an X, 24 px side margins, ≥10 px gaps.
* **One CAT client only**: never open a second connection to the radio's CAT
  bridge while the program runs (the radio answers the wrong client); use the
  program's log instead.
* **Keep the worklog**: after every change update `docs/*-WORKLOG.md` (and the
  user guide) — commit, merge into the Windows branch, push to the fork.
* Suggest a cheaper model tier for mechanical build/test loops and a stronger one
  for hard diagnosis (Claude cannot switch by itself).
* Downloads (files, firmware, manuals) need the user's OK first; put downloads in
  their own folder.

---

## 1. What piHPSDR-IF is (the project to copy from)

piHPSDR built with `make IFPAN=ON` (binary `pihpsdr-if`, app name in
`src/appname.h`, settings `~/pihpsdr-if/*.props` on the Pi /
`%LOCALAPPDATA%\piHPSDR-IF` on Windows):

* An SDR (now an MSi2500/MSi001 "RSP1A clone" through **SoapyMiri**) receives the
  TS-570D's **1st IF, 73.05 MHz** via a TSP Elettronica "IFace 2" buffer. The
  display is centred on the Kenwood's VFO read by CAT; the SDR never transmits.
* The Kenwood is driven by **CAT** (frequency, mode, PTT `TX;`/`RX;`, power `PC`,
  preamp `PA`, attenuator `RA`, ATU `AC`, AF gain `AG`, split `FT`/`FB`, subtone
  `TO`/`TN`, TX EQ `EX014000n`), meters by CAT (S, power, SWR, ALC).
* **TX audio**: microphone (local or the remote client's) → WDSP TX chain (EQ,
  leveler, CFC, compressor, TX filter, ALC) → 48 kHz → USB sound card → 600 Ω
  transformer → TS-570D **ACC2** pin 11. PTT by CAT.
* **Client / server**: server = **Raspberry Pi 5** next to the radio (SDR, CAT,
  ACC2 card, RADE engine); client = Windows (MSI) or Linux, mirror of everything,
  with microphone and speaker. Spectrum + audio UDP, control TCP. Same build
  on both sides.
* Repo: `Documents/pihpsdr-cu2ed` = branch **`cu2ed`** (Linux + shared code);
  `Documents/pihpsdr-cu2ed-win` = worktree, branch **`windows-port`** (Windows
  build + MSI). `cu2ed` is merged into `windows-port` after every change.
  Versions 3.1.N from `git describe` (N = commits since the v3.1 tag — never
  create `v*` tags; release tags like `ifpan-3.1.N`).

### Documents to read (in `Documents/pihpsdr-cu2ed/docs/`)
* `README-IFPAN.md` — overview, station diagram, document list.
* `IFPAN-USER-GUIDE.md` — every menu, toolbar function, tuning, TX, RADE, SSTV,
  touch style, client/server, logs.
* `IFPAN-WORKLOG.md` — **the full development log**: every feature, commit,
  measurement, bug and its real cause, open items. Read sections 1–5 at least.
* `RADE-IFPAN.md` — FreeDV RADE V1 inside the program.
* `IFPAN-INSTALL-LINUX.md`, `IFPAN-INSTALL-WINDOWS.md` — Pi / Windows setup,
  MSI build.
* `piHPSDR-IF-como-funciona.pdf` — Portuguese explainer (concept, hardware,
  software, screenshots).

### Feature inventory (all in the KX3 project's starting code)
| Area | Where | Notes |
|---|---|---|
| CAT thread, radio sync, tuning while the radio follows | `src/ifpan.c` (~2800 lines) | Kenwood TCP backend (`127.0.0.1:7373` socat bridge) or rigctld; "Follow Kenwood", "Clicks tune the TS", "Sync mode with TS"; display slides at once, the radio follows ~50 ms per step; resend of dropped commands; two missed answers = link down |
| IF / LO handling, DC spike offset | `src/soapy_protocol.c` (IFPAN blocks), `ifpan.c` | LO offset 250 kHz, IF offset per mode (LSB −3000 Hz) — **not needed on the KX3** |
| TX via sound card | `transmitter.c`, `ifpan.c` (`ifpan_tx_out`) | WDSP TX output real part → sound card; drive slider = radio power |
| Protections | `ifpan.c` | SWR alarm → RX by CAT; TX time-out (default 3 min); RADE power limit |
| Meters | `src/meter.c` | radio S-meter / power / SWR / ALC from CAT; dual-scale meter; touch layout sizes |
| Client / server mirror | `client_thread.c`, `server_thread.c`, `client_server.h` | all IFPAN state, RX audio resample, packet-loss readout |
| RADE V1 | `src/rade_*.c`, `rade-libs/` | docked half-screen panel, Quick Tune, callsign, 50 W limit, enter/leave restores everything |
| SSTV RX (MMSSTV receiver port, Rust `sstv-lib`) | `src/sstv_menu.c`, `sstv_glue.c`, `sstv-lib/` | docked panel, OPTIONS box, auto mode, FSK ID, Quick Tune |
| "Clean audio" for SSTV / RADE | `ifpan.c` `ifpan_clean_audio` | RX NB/NR/ANF/SNB/EQ off + AGC Long, TX EQ/CMPR/CFC/DEXP/phase rotator/mic NR off, every mode profile restored on leave; hook in `vfo_id_mode_changed`; the radio's own TX EQ OFF by CAT |
| Touch style, on-screen keyboard | `src/osk.c` | rounded buttons, X check boxes, full-width Menu, thin headers (subtitle off), Menu/Hide left of the sliders, wider S-meter, keyboard popover on any GtkEntry |
| Record WAV | `src/wav_rec.c` | toolbar `RecWAV` / MIDI, RX audio as heard, 48 kHz stereo 16-bit, `recordings/` |
| Capture / Replay | piHPSDR actions | Replay was re-enabled in the IFPAN toolbar list |
| Mic noise reduction | `src/mic_nr.c` (RNNoise) | before VOX |
| MIDI VFO | piHPSDR MIDI | encoder divisor tips in the guide |

### Hard-won lessons (do not repeat these mistakes)
* Python heredocs that write C: `\n` inside C strings becomes a real newline —
  use `chr(92)` or write the patch script with the file-write tool.
* GTK3: `gtk_widget_set_no_show_all` for overlays that `radio_reconfigure`'s
  `show_all` must not reveal; a hidden widget measures 0 (measure its child);
  CSS provider priority beats specificity (css.c uses APPLICATION; touch rules
  APPLICATION−1; Menu/Hide bold at APPLICATION+1 because the Pi's "Open Sans" has
  no bold face); GtkFixed draws in child order (re-add to raise).
* Wayland/labwc: an application cannot place its windows (the Menu opened
  right-aligned → made it full width); empty header-bar subtitle line = 68 px
  headers (`has_subtitle` off); shrink a window to its minimum, and give scrolled
  windows a min-content-height.
* Client crash (heap corruption) came from drawing with the old wider width right
  after a width change — clamp the drawing width to `rx->pixels`.
* Windows debug: `-O1 -g` build under MSYS2 gdb; copy `wdspWisdom01` next to the
  exe first. The MSI build does `make clean`.
* The radio drops CAT commands while busy (TS-570 at 25 ms steps) — 50 ms steps,
  read back, resend.
* PipeWire on the Pi may move a stream to another card when a USB device resets;
  keep USB audio/CAT on direct ports, ferrites, short cables.

### Build / deploy routine (piHPSDR-IF; the KX3 project copies it)
* **Pi** (`ssh -i ~/.ssh/hpsdr_rpi5 pi@<ip>`, Wayland/labwc): scp changed sources
  to `~/pihpsdr-if/src`, `make -j4`; launcher `~/pihpsdr-if/run-pihpsdr-if.sh`
  (log `~/pihpsdr-if.log`); screen capture
  `XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 grim /tmp/x.png`.
  When the user allows it, the Pi can be driven with `wlrctl pointer`
  (move −3000 −3000, then +x +y, click) and the app exited via Menu → Exit → Exit.
  Note: the Pi 5 moved to **192.168.1.65** on 2026-10-19.
* **Windows MSI**: `MSYSTEM=MINGW64 /c/msys64/usr/bin/bash.exe -lc 'cd <win worktree>
  && WINDOWS/installer/build-exes.sh && WINDOWS/installer/stage.sh &&
  WINDOWS/installer/build-msi.sh'` → `build-win/<name>-3.1.N-x64.msi`.
* Rust `sstv-lib` for aarch64 is built in WSL Ubuntu-24.04
  (`cargo build --release --target aarch64-unknown-linux-gnu`) and copied to
  `sstv-libs/lib/`. RADE libs: `./build-rade.sh`. RNNoise model downloaded by the
  Makefile.

---

## 2. The new project: piHPSDR-KX3

### Decisions already taken by the user (2026-11-02)
* Name **piHPSDR-KX3**.
* **New branch `kx3`** in the same fork, started from `cu2ed`, checked out in its
  own folder **`Documents/pihpsdr-kx3`** (git worktree of the same repository —
  branch names must differ between worktrees). A Windows branch for it will be
  needed too (e.g. `kx3-windows`), mirroring `windows-port`.
* Server on a **Raspberry Pi 4** (not the Pi 5) next to the KX3; **USB stereo
  sound card with line input at 96/192 kHz** receives the KX3 I/Q; Windows client
  over the LAN — otherwise the same arrangement as piHPSDR-IF.

### What is different from the TS-570D build
1. **No SDR and no IF.** The KX3 has an **RX I/Q output** (enable with
   MENU → RX I/Q ON; +10–15 mA). The sound card's left/right channels *are*
   the I/Q stream. Display span ≈ the sound card's sample rate (48k → ±24 kHz,
   96k → ±48 kHz, 192k → ±96 kHz) around the tuned frequency. Everything about
   the 73.05 MHz IF, the IFace buffer, SoapyMiri, the 250 kHz LO offset and the
   per-mode IF offset goes away.
   * Candidate path: SoapySDR's **audio** module (libaudioSupport, already on the
     Pi 5 image — check it on the Pi 4) so the existing Soapy receive path is
     reused; or a small native ALSA/PipeWire I/Q reader. Decide after testing
     latency, sample-rate support and channel order on the Pi 4.
   * To find out on the radio: whether the VFO sits exactly at 0 Hz of the I/Q
     or at an offset, I/Q swap (spectrum mirrored?), amplitude/phase imbalance of
     the KX3 + sound card (image rejection — piHPSDR has no RX IQ-balance
     correction; may be needed), the DC spike at 0 Hz (offset tuning in the KX3 or
     masking), the I/Q output level vs the card's line input.
   * The display centre = the KX3 VFO read by CAT; when the KX3 changes
     frequency the whole I/Q band moves (same physics as the IF tap).
2. **CAT = Elecraft KX3 command set** (K3/KX3 "Programmer's Reference"), over
   the KXUSB serial cable (or the KX3's ACC1/ RS-232), typically 38400 baud,
   bridged to TCP with socat as on the TS-570 (or a WiFi bridge). Kenwood-like
   core (`FA`/`FB` 11 digits, `MD`, `TX;`/`RX;`, `PC` power, `SM` S-meter,
   `AG`, `RG`, `PA` preamp, `RA` attenuator, `BW` filter, `DT` data sub-mode,
   `IF;` status, `AI`) plus Elecraft extensions (K3 extended mode `K3n`,
   `SWT` switch emulation for buttons/ATU, `TQ` TX query, meters via `BG`/`SW`…).
   **Verify every command in the Programmer's Reference and on the radio before
   using it** — do not assume TS-570 behaviour. The CAT code in `ifpan.c` is
   Kenwood-specific: refactor into a radio "backend" (TS-570 / KX3) or a KX3
   copy under `#ifdef KX3` — propose the narrower option first.
3. **TX audio into the KX3**: the KX3 has no USB audio. Options: the MIC jack
   with MIC SEL = LINE IN (and the KX3's own compression/EQ off), or DATA mode
   (DATA A) for RADE/SSTV/digital so the radio applies no processing. PTT by CAT
   (`TX;`/`RX;`). Same WDSP TX chain as piHPSDR-IF; the KX3's TX EQ ("TX EQ" menu
   / CAT) OFF in RADE/SSTV like the TS-570's EX014.
4. **Power**: KX3 0–15 W (less on some bands / with batteries), KXPA100 up to
   100 W if present — the drive slider and the RADE limit must use the KX3 scale.
   Meters: power, SWR, ALC as the KX3 reports them.
5. **Pi 4** is slower than the Pi 5: measure CPU at 192 kHz I/Q + WDSP + RADE;
   fall back to 96 kHz if needed. USB: sound card and KXUSB on direct ports.
6. Keep: touch UI, RADE, SSTV, clean audio, Record WAV, Capture/Replay, mic NR,
   client/server, MIDI VFO, packet-loss readout, protections (SWR, TX time-out).
   Remove/hide: TS-570-only functions (ATU `AC`, TS MUTE `AG`, TS TX EQ EX014,
   ACC2 notes, IF settings) — replace with KX3 equivalents where they exist.

### Suggested phases (each ends with a test the user confirms)
1. **Set-up**: worktree `Documents/pihpsdr-kx3` on branch `kx3`; build flag
   `KX3=ON` (keeping IFPAN code reusable), app name **piHPSDR-KX3**, own settings
   folder; Windows branch + MSI name. Plan the code split (shared rig-front-end
   code vs TS-570 vs KX3) and show it to the user before refactoring.
2. **Docs first**: get the user's OK to download the **KX3 Owner's Manual** and
   the **K3/KX3 Programmer's Reference** (elecraft.com) into `Documents/manuals`
   (never commit manuals). Extract: RX I/Q details, CAT commands, TX audio and
   DATA modes, power/meter commands.
3. **RX only**: Pi 4 + sound card I/Q → spectrum/waterfall/demodulation with the
   KX3 VFO in the centre (CAT read only). Measure span, image rejection, DC spike,
   I/Q swap; add IQ-balance correction if needed.
4. **CAT control**: tuning both ways (display slides, KX3 follows), mode, filter,
   preamp/attenuator, power, meters; robust resend like `ifpan.c`.
5. **TX**: sound card → KX3 (LINE IN or DATA A), PTT by CAT, drive = power,
   SWR protection, TX time-out — dummy load, user's OK per test.
6. **Client/server, RADE, SSTV, Record WAV, touch** — verify each on the Pi 4
   screen and the Windows client; docs (README, user guide, worklog, install) in
   English; MSI.

### Open questions to ask the user when they come up
* Which USB sound card (model, 96 or 192 kHz, line-in level), and which card
  outputs TX audio to the KX3 (same card or another)?
* CAT cable: KXUSB on the Pi 4, or a WiFi bridge? Baud rate set in the KX3?
* KXPA100 / ATU (KXAT3) fitted?
* The Pi 4's address and OS (Raspberry Pi OS Bookworm/Trixie, desktop with
  labwc like the Pi 5?), and whether the touch screen is the same 1024x600.

Start by reading the documents in section 1, then propose phase 1 in detail
(folder, branch, build flag, code split) and wait for the user's go-ahead.
