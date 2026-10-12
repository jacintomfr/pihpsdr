# piHPSDR-IF — user guide (TS-570D)

How to operate piHPSDR-IF with the Kenwood TS-570D. Everything not listed
here works as in piHPSDR. Install: `docs/IFPAN-INSTALL-LINUX.md`,
`docs/IFPAN-INSTALL-WINDOWS.md`. Overview: `docs/README-IFPAN.md`.
Portuguese version: `docs/IFPAN-GUIA-UTILIZADOR.md`.

## 1. The idea in one paragraph

The SDR does not receive the antenna: it receives the TS-570D's **first IF
(73.05 MHz)** through an IF buffer. So the spectrum is always "what the
Kenwood sees" around its own frequency, and the centre of the display is
the Kenwood's VFO (read by CAT). piHPSDR-IF demodulates that IF with WDSP
(its own filters, NR, AGC, RADE...), controls the Kenwood by CAT
(frequency, mode, PTT, power, preamp, attenuator, tuner, speaker) and sends
its processed TX audio into the Kenwood's ACC2 input. The SDR never
transmits.

Consequence: **when the Kenwood changes frequency, the whole IF moves.**
Turning the TS dial always slides the spectrum; no setting can stop that.

## 2. Radio menu — "Kenwood / IF panadapter" block

| Control | What it does |
|---|---|
| **Follow Kenwood** | On: the receiver always sits on the Kenwood frequency (turning the TS dial tunes piHPSDR-IF too). Off: when the TS dial turns, the receiver keeps the station it was on (CTUN offset). |
| **Clicks tune the TS** | On: every tuning step made in piHPSDR-IF (mouse wheel, clicks, MIDI VFO, keypad) is sent to the Kenwood; the display slides at once (section 4). Off: the spectrum stays still and only the receiver moves inside the span; the Kenwood stays where it is ("TS here" brings it over). |
| **Sync mode with TS** | Mode kept the same both ways (Kenwood ↔ piHPSDR-IF). DIGU / DIGL (RADE, FT8...) go to the Kenwood as USB / LSB (ACC2 audio). |
| **Mute when TS in TX** | RX audio muted while the Kenwood transmits. |
| **SDR LNA off** | Keeps the SDR's front-end LNA off (MSi001: +24 dB, switched in by libmirisdr above about RF 80). The IF buffer signal is strong; with the LNA the clone overloads. The RF slider then works on the other stages. |
| **SDR LO offs. (kHz)** | 0..250 kHz. Tunes the SDR away from the IF so its DC spike and mirror land outside the display. MSi2500 clone: **250 kHz at 384k span** puts both out of sight (at 768k they come back weaker). |
| **IF offs. \<mode\> (Hz)** | Per Kenwood mode (shown for the current mode): the TS-570D moves its carrier inside the IF with the mode. Set once per mode with a known station. Found: **LSB = −3000 Hz**, USB 0. CW / CW-R / AM / FM still to check. |
| **TX audio to TS** | Sound card output that feeds the Kenwood's ACC2 (server only). |
| **CAT** | "Kenwood TCP" (address `127.0.0.1:7373`, the socat / ESP32 bridge) or "via rigctld" (`127.0.0.1:4532`). Server only; the client has no CAT link of its own. |

Other settings: the IF frequency is `ifpan.if_freq` in the props file
(default 73050000); frequency calibration (ppm) in the Radio menu applies
to the IF LO. The SDR gains (RF slider; "RX1 Gains" per stage) are in the
right-hand column.

## 3. Toolbar functions (assign them in the toolbar / MIDI menus)

| Function | Button | What it does |
|---|---|---|
| MOX / PTT | MOX | keys the Kenwood by CAT (`TX;` / `RX;`; `RX;` also on exit). Refused without CAT. |
| TS to RX freq | **TS here** | tunes the Kenwood to the frequency piHPSDR-IF receives on (use with "Clicks tune the TS" off, before transmitting). |
| Preamp | **PRE** | Kenwood preamp (`PA`). Shown in the VFO bar. |
| TS Atten On/Off | **ATT** | Kenwood attenuator (`RA`). Shown in the VFO bar. |
| TS Tuner On/Off | **ATU** | antenna tuner in / through (`AC 10;` / `AC 00;`). |
| TS Tuner Tune | **ATU tune** | starts a tuning cycle (`AC 11;`). Status line: ATU ON / THRU / TUNING. |
| TS Speaker Mute | **TS MUTE** | Kenwood AF gain to 0 and back (`AG`), to compare its audio with piHPSDR-IF's without touching VOL. Turning VOL up ends the mute; the level is restored on exit. Red "TS MUTE" on the status line. |
| Tune | TUNE | 1 kHz tone into ACC2 at "Tune power (W)" (TX menu); the Kenwood's PWR is restored afterwards; CW / FSK switched to USB / LSB for the tune. |
| Tune Drive | TUNE_DRIVE | tune power slider. |
| Two-Tone | TWO_TONE | two-tone test at the TX Drive power, same mode switch. |
| RADE Menu | **RADE-M** | opens the RADE menu. |

## 4. Tuning

**Recommended (also the remote way of working): tune from piHPSDR-IF, not
from the TS dial.** Mouse wheel, clicks, keypad or a **MIDI VFO knob**
(Linux/Pi build; e.g. the Arduino Due controller with a 600 PPR optical
encoder). Settings: "Clicks tune the TS" **on**, "Follow Kenwood" on, CTUN
on.

What you see (since 2026-10-12): the filter stays in the centre, the
spectrum and the waterfall slide at once, like a normal radio. The Kenwood
follows by CAT a few hundred milliseconds later; until then a strip at one
edge stays empty and the dashed Kenwood line shows where the TS really is.
When it arrives, the real IF data fills in — no jump back. (If the
Kenwood does not answer within 2 s the display shows the real position
again.) A click on the spectrum during the slide (< 1 s) lands with the
shifted scale.

* **Span:** 96k gives the most detail around the station and is fine now
  that the display slides; 384k (+ zoom) to watch a whole band. With the
  MSi2500 clone keep ≤ 384k (LO offset 250 kHz).
* A large step at 96k: the TS-570D mutes its receiver while its PLL
  relocks, so the new part of the band can "rise from the bottom" for a
  moment; lower the spectrum averaging (Display menu) to shorten it.
* **MIDI VFO speed:** Radio menu "VFO Encoder Divisor". A high-resolution
  encoder (e.g. 600 pulses per turn) at a 100 Hz step and divisor 1 moves
  60 kHz per turn, faster than the TS-570 follows by CAT, and the display
  slides in jumps; raise the divisor (the user found up to 60 fluid) or
  lower the step until it is fluid. The divisor no longer slows the CAT
  readings of the Kenwood (always every 50 ms).
* The mouse wheel sends each target to the Kenwood again (up to 3 times)
  because the TS-570D drops commands that arrive while it is busy.
* Band menu: TS-570 bands (100 kHz – 60 MHz); a band change sends the
  frequency and then the band-stack mode (~300 ms later, read back).
* **Turning the TS dial** still works; the spectrum slides with it
  (physics: the Kenwood's own LO moves the IF).

## 5. Transmitting

* **TX Drive slider = Kenwood output power** (`PC`), 5 W steps, 5..100 W
  (AM 5..25 W, kept per mode by the radio). The slider follows the radio.
  Drive 0 = no audio into ACC2.
* The microphone (local, or the remote client's) goes through the WDSP TX
  chain (mic gain, EQ, leveler, CFC, compressor, TX filter) and the
  processed audio goes to ACC2. AM / FM: processed as DSB audio, the
  Kenwood makes the AM / FM. TX filter about 200–2800 Hz for the TS's
  2.7 kHz filter.
* **Mic noise reduction**: TX menu, DEXP page, "Mic Noise Reduction
  (NR3)" (RNNoise, before VOX). VOX works, also from a client.
* **SWR protection**: TX menu "SWR alarm at" / "SWR Protection": two
  Kenwood SWR readings at or above the alarm → the Kenwood is dropped to RX
  at once by CAT, MOX off, drive 0 (skipped while the ATU tunes; during
  TUNE after 10 s).
* Meters: Kenwood power / SWR / ALC read by CAT on piHPSDR's TX meter.
* **Not yet tested on air** (the ACC2 plug was missing): TUNE / TwoTone
  power, ALC level, CW → USB during TUNE, FM deviation, SWR protection in
  TUNE, RADE TX.
* TS-570D side for ACC2 audio: speech processor off, TX EQ flat, ALC low;
  set the level with the sound card output volume.

### 5.1 ACC2 wiring and TS-570D menus

From the TS-570 instruction manual ("MCP and TNC", ACC2 table, and the
menu table; checked 2026-10-12 in the Italian edition, pages I-62 and
the menu list). ACC2 is a **13-pin DIN** on the rear panel.

| Pin | Name | Function |
|---|---|---|
| 1 | NC | not connected |
| 2 | RTK | RTTY keying input |
| 3 | ANO | AF output from the transceiver (independent of the AF knob; level menu 34; 4.7 kΩ) |
| 4 | GND | shield for pin 3 |
| 5 | PSQ | squelch control (open: low impedance) |
| 6 | SMET | S-meter output |
| 7 | NC | not connected |
| 8 | GND | chassis ground |
| 9 | PKS | PTT for a TNC / MCP — **the front microphone audio is muted** while keyed this way |
| 10 | NC | not connected |
| **11** | **PKD** | **audio input ("microphone audio input": connect the TX data / audio here)** |
| **12** | **GND** | **shield for pin 11** |
| 13 | SS | PTT in parallel with the MIC jack (foot switch etc.) |

**piHPSDR-IF TX audio:** the sound card output through the 600 Ω isolation
transformer → **pin 11 (hot)** and **pin 12 (ground / shield)**. Nothing
else is needed: PTT is by CAT (`TX;` / `RX;`), the receive side is the IF
tap. (Pins 3 / 4 would give the RX audio, not used.)

**Menus** (press MENU, choose the number with the dial / [UP] [DOWN]):

| Menu | Function | Values (default) | Setting for piHPSDR-IF |
|---|---|---|---|
| **33** | AF **input** level for digital operation (ACC2 pin 11), all modes except CW / FSK | 0 (minimum) .. 2 (maximum) (**2**) | start at 0 or 1; fine level with the sound card output volume so that the ALC barely moves |
| 34 | AF **output** level on ANO (pin 3) | 0 .. 9 (4) | not used |
| **32** | filter bandwidth for digital operation (SSB / FM) | OFF / narrow filters (**OFF**) | **OFF** for voice and RADE (the filters are too narrow for speech; with a filter on, "FSK" shows next to the mode) |

Other settings from the manual's digital-operation notes: **speech
processor OFF** (and for RADE also TX EQ flat, ALC low), SSB / FM modes.
TS-570 CAT has only `TX;` (no choice of TX audio source as on the TS-590's
`TX1;`), so it is not documented whether the front microphone stays live
together with pin 11 when keyed by CAT; only PKS (pin 9) is said to mute
it. **Check on the dummy load:** key by CAT with the microphone plugged in
and speak into it — if it is heard on the monitor, unplug the microphone
(or turn MIC gain to minimum) for ACC2 / remote operation.

Low-frequency response: the ACC2 input was designed for data audio and is
reported (NU9N, for the TS-570 / TS-870 family) to cut low frequencies;
a modification (C131 to 4.7 µF on the TS-570) is described there for fuller
voice through ACC2. Not done here; only if the transmitted voice sounds
thin.

## 6. RADE (FreeDV RADE V1)

Runs on the server; the client's RADE menu (RADE-M) remote-controls it.
Selecting RADE puts the receiver in DIGU / DIGL (Kenwood USB / LSB) with
the right filter, also on the client. RX confirmed on air (SYNC at ~21 dB
SNR, callsigns decoded; signals under about +3.8 dB SNR do not decode the
callsign). Own callsign: RADE menu "My callsign". VOX does not work with
RADE — use PTT / MOX. Details: `docs/RADE-IFPAN.md`.

**No sound in the headphones with RADE on?** With RADE enabled only the
decoded speech goes to the speaker: with no RADE station on the air it stays
(almost) silent. Two options in the RADE menu decide what you hear meanwhile:
**"Mute analog audio"** (on: the ordinary band audio — noise, SSB — is cut
while no RADE is decoded; off: you hear the band until a RADE station
appears) and **"Squelch gate"** (on: the audio closes on weak RADE
signals). Untick both to always hear something; tick them to hear only the
decoded speech. (Seen on the Pi 5, 2026-10-12: "no audio" was simply RADE
switched on.)

## 7. Server and client

* The **server** sits next to the radio (now the Raspberry Pi 5): SDR, CAT,
  ACC2 sound card, RADE engine.
* The **client** (Windows or Linux) mirrors everything: Kenwood state, all
  IF panadapter options, TS MUTE, PRE / ATT / ATU, RADE menu, power, PTT.
  Its microphone goes to the server, the RX audio comes back. Latency on
  the status line (0–1 ms on a LAN).
* Both must run the **same build**.
* The RX audio is UDP: a lost packet is an audible gap (~0.5 s cuts =
  packet loss on the LAN: check cables / switch, `ifpan-tools/net/`).
* Server option "Server runs protocol only if client is connected": off if
  the server's own screen is used too.

## 8. SDR notes

| SDR | Notes |
|---|---|
| **MSi2500 / SDRplay RSP1A clone** (recommended) | SoapyMiri, 1.536 MS/s isochronous, 14-bit; audio "equal to the Hermes-Lite 2". LO offset 250 kHz at 384k, SDR LNA off, direct USB port. Windows driver libusbK. On the Windows PC: ground-loop stall at ≥ 25 W (install guide A6); not on the Pi. |
| RTL-SDR R820T (TCXO) | 1.536 MS/s, WinUSB on Windows. Worked well; less dynamic range. |
| PlutoSDR | first SDR used; weaker at 73 MHz. |

The SDR is reopened automatically when it drops off USB (~1.5–2 s; after a
stall during TX, at once when the Kenwood is back in RX).

## 9. Logs and diagnostics

* Log: Windows `%LOCALAPPDATA%\piHPSDR-IF\pihpsdr.stdout`; Pi
  `~/pihpsdr-if.log`.
* `cat_thread:` lines: CAT state, PTT, mode, tuner, TS MUTE.
* `audio diag:` (3.1.177+): server send gaps, client arrival gaps, output
  buffer dry / overrun.
* `mic diag:` (every 5 s, Windows): mic callbacks / samples / SDR
  heartbeats.
* An empty file `ifpan-txdump` in the settings folder at server start
  records `txdump_mic.f32` / `txdump_out.f32` (30 s, float32, 48 kHz)
  while transmitting.
