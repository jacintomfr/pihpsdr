# RADE V2 test build (branch `pihpsdr-if-rade2`)

**Experimental.** RADE V2 is still under development upstream (FreeDV /
drowe67 radae, freedv/rade_c): "on-air use is not recommended at this
stage"; the waveform, weights and API may change without notice and later
versions will not be compatible. This branch exists only for on-air tests
between two stations that both run builds from **the same rade_c commit**.
The normal piHPSDR-IF (branch `cu2ed` / `windows-port`) stays RADE V1.

## What V2 promises (radae README, comparison table)

| | V1 | V2 |
|---|---|---|
| 99 % occupied bandwidth | ~2100 Hz | ~860 Hz |
| PAPR (100 % CCDF) | 4.2 dB | 3.5 dB |
| Threshold SNR, AWGN | -2 dB | ~-4.5 dB |
| Threshold SNR, MPP | 0 dB | ~-3 dB |

## What the branch changes

* `Makefile`: `RADE_V2?=ON` adds `-D RADE_V2` (build with `RADE_V2=OFF` for
  V1).
* `src/rade_engine.c`: `rade_open(..., RADE_MODE_V2)`. V2 has no
  End-of-Over callsign bits (`rade_n_eoo_bits()` = 0): the EOO frame still
  ends the over (squelch / unkey work as before), but **no callsign is sent
  or decoded** (the received-callsigns list stays empty; the V2 aux text
  channel is not used yet). The library's TX band-pass (V1 only) is not
  touched. Sizes are taken from the library (V2: 144 features / 320 modem
  samples per 40 ms frame, EOO 960 samples, nin max 200).
* `src/rade_menu.c`: the RADE menu title says "RADE V2 (experimental)".
* Filters (`src/rade_glue.h`): the V2 signal measured from the library
  (8 s of transmit output, real part as sent through SSB): peak 1450 Hz,
  99 % of the power in **1080 - 1900 Hz (820 Hz)**, -20 dB at 1100 - 1900 Hz
  (V1: 99 % in 130 - 3240 Hz, -6 dB at 730 - 2220 Hz). V2 TX filter
  **1000 - 2000 Hz**; RX "Fit Filter" narrow **950 - 2050 Hz**, wide
  **800 - 2200 Hz** (room for a mistuned station). V1 builds keep 700 - 2300
  (TX), 700 - 2300 / 300 - 2700 (RX).
* Everything else (RADE menu enter / leave, 50 W limit, TX chain) is as in
  V1.

## rade_c version (must match the other station)

`build-rade.sh`: https://github.com/jacintomfr/rade_c branch `hpsdr-rs-local`,
commit `c394514738bc407930f80ccdcd28fc8531c28a1b` (V1 + V2). The deskHPSDR
test branch `deskhpsdr-rade2` (jacintomfr/deskhpsdr) uses the same commit.

## Loopback check (2026-10-13, Pi 5)

A small program (`rade_tx` -> real part -> `rade_rx`, 100 frames + EOO)
synchronised and decoded in both modes: V1 97 frames + EOO, V2 96 frames +
EOO (SNR estimate 17 dB with no noise added).

## Pi 5 install (separate from the normal one)

`~/pihpsdr-if-rade2` (a copy of `~/pihpsdr-if` with its own settings, log
`~/pihpsdr-if-rade2.log`), desktop shortcut "piHPSDR-IF RADE V2 (test)".
Only one of the two may run at a time (same SDR and CAT). The Windows
client does not change (the RADE engine runs on the server).
