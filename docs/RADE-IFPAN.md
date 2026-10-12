# RADE V1 in piHPSDR-IF

Maintainer reference for CU2ED. RADE V1 (FreeDV "radio autoencoder") is ported
from the deskHPSDR Windows RADE port (`Documents/deskhpsdr`, branch
`dekhpsdr-win-rade`, `docs/RADE-INTEGRATION.md`), which itself ports the
hpsdr-rs Rust implementation. This file only describes what is specific to
piHPSDR-IF; the engine, signal flow, EOO codec, mic conditioning, menu and
test notes in the deskHPSDR document apply unchanged.

## 1. Status

* Phase 1: RADE on the server, local operation (RX and TX), Windows and
  Linux builds. RX confirmed on air 2026-10-11 (14.236 MHz, SYNC at about
  21 dB SNR, EOO callsign CU2FO decoded, "audio very good"). TX needs the
  ACC2 cable (not yet tested).
* Phase 2: client/server — the RADE menu on a client acts on the server's
  engine and shows its state (section 7).
* Phase 3: FreeDV Reporter (the deskHPSDR `rade_reporter.c` client).
* Phase 4: Raspberry Pi build (aarch64, NEON) and CPU measurement. Built
  and running on the Pi 5 server since 2026-10-11
  (`docs/IFPAN-INSTALL-LINUX.md` section 3; RX without RADE ~23 % of the
  four cores); CPU while decoding RADE still to measure.

## 2. Architecture decision: RADE runs on the server

The server is the side that owns the radio (Kenwood via ACC2 + CAT, the
RTL-SDR on the IF). RADE runs there, as FlexRadio runs its FreeDV/RADE
waveform next to the radio (in the radio, or on a Pi on the radio LAN) and
OpenWebRX+ decodes RADE on the server.

* The modem audio never crosses the network: jitter / loss on the client
  link only touches speech (a short glitch), not the OFDM modem (a sync
  loss, a lost End-of-Over callsign).
* One sound-card clock for the modem (the ACC2 output) and PTT / EOO timing
  in one place (the unkey intercept below runs before the Kenwood is
  unkeyed by CAT).
* The client sends its microphone as ordinary audio (RNNoise on the client
  is fine: it only cleans the speech) and receives the decoded speech as
  ordinary RX audio. No change in the client/server protocol for phase 1.

CPU (web research, 2026-10): the FreeDV project states RADE V1 runs on a
Raspberry Pi 4 and above; the FlexRadio waveform (C port) uses about 30 % of
one Pi 4 core while not decoding; RX (the FARGAN vocoder, ~300 MMAC/s) is
the expensive part. The hpsdr-rs kiosk runs RADE on a Pi 5. A Pi 4 server
also runs the RTL-SDR, WDSP RX/TX and GTK: to be measured (phase 4); the
Pi 5 is the fallback.

## 3. Files

New (copied unchanged from deskHPSDR): `src/rade_engine.c/h`,
`rade_text.c/h`, `rade_shim.c/h`, `rade_mic_agc.c/h`, `rade_denoiser.c/h`,
`rade_resample.c/h`, `rade/CMakeLists.txt`, `rade/src/opus-nnet.[ch].diff`.

Adapted: `src/rade_glue.c/h`, `src/rade_menu.c/h`, `build-rade.sh`
(Linux generator, parallel make).

Hooks (all inside `#ifdef RADE`):

| File | Where | What |
|---|---|---|
| `receiver.c` | `rx_process_buffer()`, before the output loop | `rade_glue_rx_block()`: RX1 audio -> decoded speech, before capture, the remote client, TCI and the speaker |
| `transmitter.c` | `tx_add_mic_sample()`, after the remote / local mic and the IFPAN mic NR, before capture / VOX / the chain | `rade_glue_tx_sample()` replaces the mic sample by modem audio while TX is armed |
| `transmitter.c` | TX audio monitor | off while armed (no modem tones in the monitor) |
| `radio.c` | `radio_set_mox()`, right after the client branch | MOX-off intercept (EOO first, real MOX-off at most 2 s later), before the client is told and before `ifpan_ptt()` unkeys the Kenwood |
| `radio.c` | server part of `radio_restore_state()` / `radio_save_state()` | `rade_glue_load_state()` / `rade_glue_save_state()` (runs before receivers exist) |
| `radio.c` | `radio_stop_program()` after `ifpan_stop()` | `rade_glue_shutdown()` (forced TX values back before saving, engine thread joined) |
| `new_menu.c/h` | TX column, after VOX | button "RADE", `start_rade_menu()` |
| `actions.c/h` | IFPAN actions | `MENU_RADE` ("RADE-M", toolbar / MIDI) |
| `ifpan.c` | `kenwood_mode_index()`, `mode_apply()` | DIGU / DIGL sent to the Kenwood as USB / LSB (ACC2 audio); a Kenwood USB / LSB does not move the receiver out of DIGU / DIGL |
| `Makefile` | RADE section | RADE on when `rade-libs/lib/librade_static.a` exists (IFPAN builds), `RADE=OFF` to disable |

## 4. Differences from deskHPSDR

* **Own callsign:** piHPSDR has no "own callsign" setting; it is edited in
  the RADE menu ("My callsign", upper case, no spaces, 8 characters) and
  saved as `rade_callsign`.
* **TX chain forcing:** piHPSDR has no separate leveler switch (the leveler
  follows compressor / CFC), and the TX filter is `default_filter_low/high`
  (plus `use_rx_filter`). While armed: compressor, CFC, CFC-EQ, TX EQ off,
  `use_rx_filter` off, TX filter 700-2300 Hz. The setters store these in the
  TX mode's profile (DIGU / DIGL); the user values are put back on disarm
  and at shutdown.
* **Kenwood mode:** DIGU / DIGL go to the TS-570D as USB / LSB (before: FSK-R
  / FSK, which are keyed by the FSK line and ignore ACC2 audio). With "Sync
  mode with TS" off the Kenwood must be put in USB / LSB by hand.
* **Quick Tune** tunes the Kenwood by CAT (`ifpan_band_changed`, frequency +
  USB / LSB); the display follows the radio.
* **Settings** are saved when the RADE menu closes.
* **Client:** the same RADE menu, acting on the server (section 7).
* **No FreeDV Reporter** yet (phase 3).

## 5. Operating notes (TS-570D)

* TS-570D: speech processor (PROC) OFF, TX EQ flat, ALC low: the RADE
  waveform (PAPR about 4 dB) must not be compressed. Set the level into ACC2
  with the sound card output volume so that the ALC barely moves.
* VOX does not work with RADE (it would see the modem audio): use PTT / MOX.
* The TX Drive slider is the Kenwood PWR (5 W steps).
* SWR protection drops the Kenwood to RX by CAT at once; the RADE unkey
  intercept only delays piHPSDR's own MOX-off.

## 6. Build

Libraries (once; network needed for Opus and its DNN model):

```
# Windows: MSYS2 MINGW64 shell, packages as in deskHPSDR
#   (cmake make patch git curl autoconf automake libtool gettext-devel,
#    mingw-w64-x86_64-speexdsp)
# Linux / Raspberry Pi: cmake autoconf automake libtool gettext libspeexdsp-dev
RADE_C_DIR=/path/to/rade_c ./build-rade.sh     # rade_c commit c394514 (private repo)
```

`rade_c` is the private `jacintomfr/rade_c` (branch `hpsdr-rs-local`, commit
`c394514`); `hpsdr-rs-git/vendor/rade_c` is the same commit. The Windows
libraries of deskHPSDR (`deskhpsdr/rade-libs`, same toolchain) can be copied
instead of rebuilding. `rade-libs/`, `rade-src/`, `rade-build/` are ignored
by git.

Then `make` (RADE is switched on automatically). Checked on 2026-10-11:
no symbol clashes between WDSP, the RNNoise of the IFPAN build, libopus and
librade (nm); WDSP has its own `nnet.h`, so the Opus include dirs come
before `$(INCLUDES)` for `rade_engine.o` / `rade_shim.o`. The binary grows
by about 30 MB (weights).

## 7. Client / server

The engine only runs on the server; the client's RADE menu is a remote
control of it (protocol version 0x013100F3, both ends must be the same
piHPSDR-IF build):

* **INFO_RADE** (server -> client, UDP, with the periodic data every 150 ms,
  `RADE_DATA` in `client_server.h`): flags (RX enabled, TX armed, denoiser,
  leveler, compressor, EQ, mute analog, squelch, wide filter, engine
  running), SYNC / SNR / offset / level / EOO count / dropped, the four mic
  EQ gains, the own callsign and the last 8 received callsigns with SNR.
  Packed by `rade_glue_fill_remote()`, applied by `rade_glue_client_update()`.
* **CMD_RADE** (client -> server, TCP, `RADE_COMMAND`): b1 = `RADE_REMOTE_*`,
  b2 = on / off, EQ band or Quick Tune index, s1 = EQ gain (dB * 10), text =
  callsign. Carried out by `rade_glue_remote_command()` as if done in the
  server's own menu. The server always consumes the bytes (also a build
  without RADE), so the TCP stream stays in step.
* In `rade_glue.c` the setters on a client send the command and update the
  local copy at once; incoming settings are ignored for 0.6 s after a local
  change (`REMOTE_HOLD_US`) so a packet sent before the server saw the change
  does not flip a check box back. The getters read that copy, so the menu
  code is the same on both sides; on a client the menu also follows changes
  made in the server's own menu.
* Quick Tune and Fit Filter moved from the menu into the glue
  (`rade_glue_quick_tune`, `rade_glue_fit_filter`) so that they always run
  on the server (mode, Kenwood by CAT, Var1 filter).
* The client's microphone reaches the server as ordinary remote TX audio
  and the decoded speech comes back as ordinary RX audio: nothing RADE
  specific on the audio path. Settings are saved by the server (its props),
  when the server's RADE menu closes and at exit.
