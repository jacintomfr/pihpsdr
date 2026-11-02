# piHPSDR-KX3 — KX3 facts from the manuals (phase 2)

Source documents (downloaded 2026-11-02 from ftp.elecraft.com with the user's OK,
kept in `Documents/manuals/kx3/` with text dumps — **never committed**):
KX3 Owner's Manual Rev C5 + errata C5-4, K3S/K3/KX3/KX2 Programmer's Reference
Rev G5, KX3 schematic (Dec 2012), KXAT3 option manual.

Everything below is from those documents; items marked **(radio)** must still be
checked on the user's KX3.

## 1. RX I/Q output (Owner's Manual pp. 5, 26, menu p. 39)

* **2.5 mm jack "RX I/Q"**: quadrature outputs of the receive mixer (I, Q).
  Enable with **MENU: RX I/Q = ON** (+10–15 mA receive current).
* Into a **stereo line input** (mic inputs are mono — not usable). Cable 2.5 mm
  plug → 3.5 mm stereo.
* The spectrum is centred on the KX3's tuned frequency: 48 kHz sampling → ±24 kHz,
  96 kHz → ±48 kHz, 192 kHz → ±96 kHz. (Exact sub-kHz offset, I/Q order /
  mirror and image rejection: **(radio)**, phase 3.)
* Roll-off away from the centre: **−2.5 dB at ±24 kHz, −4 dB at ±48 kHz,
  −7 dB at ±96 kHz** (noise floor included). A display flattening curve may be
  added later; demodulation is unaffected in practice.

## 2. CAT (Programmer's Reference G5)

Transport: ACC1 jack via **KXUSB** (USB) or KXSER (RS-232). Baud rate =
**MENU: RS232**, default **4800**, choices 4800 / 9600 / 19200 / 38400 (`BRn`
n = 0–3). Use **38400**. Typical response < 10 ms, worst ~100 ms, band changes up
to 500 ms (commands deferred meanwhile). Busy state answers `?;` (same handling
as the TS-570's refusals). Kenwood-like syntax: commands end with `;`, GET = bare
command.

| Need | Command | Format / notes |
|---|---|---|
| VFO A / B | `FA` / `FB` | 11 digits in Hz; Hz digit ignored unless FINE (1 Hz) tuning (`SWT49`); a different band → band change ~0.5 s; linked VFOs: FA also sets B |
| Mode | `MD` | 1 LSB, 2 USB, 3 CW, 4 FM, 5 AM, **6 DATA**, 7 CW-REV, 9 DATA-REV |
| Data sub-mode | `DT` | 0 **DATA A**, 1 AFSK A, 2 FSK D, 3 PSK D (use only in DATA mode) |
| PTT | `TX;` / `RX;` | `TX` is ignored in FSK D / PSK D (not used by us) |
| TX state | `TQ` | `TQ0;` / `TQ1;` — preferred over `IF` |
| Status | `IF` | freq, RIT/XIT, TX flag, mode, VFO, split… |
| Power setting | `PC` | KX3 without KXPA100: **000–012 W on 80–20 m, 000–015 W on 160 / 15–6 m**; with KXPA100: 000–110 W. K22 extended format allows 0.1 W units |
| Actual power | `PO` | tenths of a watt (QRP) or watts (QRO, KXPA100); estimated from drive |
| SWR | `SW` | `SWnnn;` tenths, 010–999; valid in TX, TUNE and ATU tuning |
| S-meter | `SM` | 0000–0015 (S9 = 6, S9+20 = 9, +40 = 12, +60 = 15); 0000 in TX |
| Bargraph | `BG` | S in RX, power / ALC in TX |
| Preamp | `PA` | 0 / 1 |
| Attenuator | `RA` | 00 / 01 (−10 dB) |
| AF / RF gain | `AG` / `RG` | AF 000–255 |
| Bandwidth | `BW` | 10 Hz units |
| Mic gain | `MG` | 000–060 |
| Compression | `CP` | 000–040 |
| TX EQ | `TE` | **SET only**: 8 bands ×3 chars (−16…+16 dB, 50 Hz…3.2 kHz); applies to SSB (also CW / DATA tx) — no GET, so the user's setting cannot be read back (as on the TS-570) |
| ATU | `SWT44` (ATU TUNE tap), `SWH44` (ANT hold); `AK` reads the L/C network | KXAT3 |
| Switch emulation | `SWTnn` / `SWHnn` | KX3 table 8 (e.g. XMIT 16, MODE 14, DATA 17, PRE 19, ATTN 27) |
| Auto-info | `AI0`–`AI3` | AI0 default (poll) — we poll like on the TS-570 |
| Power off | `PS0;` | never sent by us |

## 3. TX audio and data modes (Owner's Manual pp. 5, 18, 27, menus)

* No USB audio in the KX3: audio in through the **MIC jack** (tip = mic audio,
  ring1 = PTT, ring2 = logic ground). From a sound card line output it needs an
  attenuator to mic level and **MIC BIAS = OFF** (bias is for the MH3 mic) —
  details **(radio)**.
* **DATA A**: "disables compression and RX/TX EQ", USB by default — the KX3's own
  equivalent of our "clean audio" for RADE / SSTV / digital. DATA level is set on
  the ALC scale (p. 18).
* The manual recommends **5.0 W or less in all data modes** (the KX3 reduces power
  when hot). → the RADE power limit becomes **5 W** on the KX3 (TS-570: 50 W).
* Voice SSB: mic gain `MG`, compression `CP`, TX EQ `TE` / menu TX EQ. Our
  WDSP chain does the processing, so CMP 0 and a flat TX EQ on the KX3 in SSB too
  (to confirm with the user).
* ACC2 jack: keyline output (low in TX) and a GPIO (TX inhibit or PTT input,
  MENU: ACC2 IO). Not needed: PTT is by CAT.

## 4. Consequences for the code split (docs/KX3-CODE-SPLIT.md)

* `el_*` CAT backend: `FA`, `MD`/`DT`, `TX`/`RX`/`TQ`, `PC` (KX3 band-dependent
  range, maybe 0.1 W via K22), `PO` + `SW` for the TX meters, `SM` for the RX
  meter, `PA`, `RA`, `AG`, ATU via `SWT44`.
* Modes: RADE / SSTV / digital use **MD6 + DT0 (DATA A)** (or MD9 for LSB) instead
  of the TS-570's USB/LSB; no `EX014` (TS TX EQ) needed in DATA A.
* Power: drive slider 0–15 W (0–12 W on 80–20 m); RADE / data limit 5 W.
* RX: 192 kHz card → ±96 kHz with −7 dB at the edges; 96 kHz may be the better
  compromise on the Pi 4 (CPU) — to measure.
