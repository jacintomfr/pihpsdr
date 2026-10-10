# TS-570D RS-232 ↔ WiFi bridge (ESP32-S3-Zero)

Replaces the USB-RS232 adapter, whose USB link put interference (vertical
lines) on the receiver, by a WiFi link. The ESP32 passes the raw CAT byte
stream between the TS-570D COM port and a TCP port.

```
TS-570D COM (DB9) ── MAX3232 module ── ESP32-S3-Zero UART1 ── WiFi ── TCP 7373
```

## Parts

- Waveshare **ESP32-S3-Zero**
- **MAX3232** RS-232/TTL module, preferably with a DB9 connector. It must be a
  **MAX3232** (works at 3.3 V); a MAX232 needs 5 V and must not be used with
  the ESP32.
- Power: 5 V into the ESP32-S3-Zero (5V pin or USB-C). From the radio's
  13.8 V use a **linear** regulator (e.g. 7805 with heat sink), not a cheap
  switching buck converter, which radiates on HF.
- Metal box, ferrite on the DB9 cable and on the power lead.

## TS-570D COM connector (manual, appendix I-70)

Pin names are from the computer's point of view:

| DB9 pin | Name | Direction at the radio | Function |
|---|---|---|---|
| 2 | RXD | output | data sent by the radio |
| 3 | TXD | input  | data received by the radio |
| 5 | GND | —      | signal ground |
| 7 | RTS | input  | when **low**, the radio does **not** send data |
| 8 | CTS | output | radio ready to receive |

## Wiring

| ESP32-S3-Zero | MAX3232 | TS-570D DB9 |
|---|---|---|
| GP5 (UART TX) | T1IN → T1OUT | pin 3 (radio input) |
| GP6 (UART RX) | R1OUT ← R1IN | pin 2 (radio output) |
| GP7 (RTS, held active) | T2IN → T2OUT | pin 7 (RTS input) |
| GND | GND | pin 5 |
| 3V3 | VCC | — |

**Module with only one channel (TX/RX):** leave GP7 unconnected and bridge
**pins 7 and 8** at the radio's DB9 plug: the radio's own CTS output then
holds its RTS input active.

DB9 modules are wired in different ways (some swap pins 2 and 3). Before
connecting the radio, measure with a multimeter: an RS-232 **output** idles at
about −5 to −10 V; an unconnected input reads about 0 V. The radio's pin 2
(output) must go to the module's receiver input (R1IN), never to another
output.

## Radio

Menu 35 = **"576-1"** (57600 bps, 1 stop bit). Switch the radio off and on
after changing it.

## Firmware

Arduino IDE with the Espressif "esp32" core (2.x or 3.x):

- Board: **ESP32S3 Dev Module**
- USB CDC On Boot: **Enabled** (debug log over USB at 115200)
- Flash Size: 4MB

Edit the configuration block at the top of `ts570_wifi_bridge.ino`:
`WIFI_SSID`, `WIFI_PASSWORD`, and preferably a fixed IP (`USE_STATIC_IP`,
`STATIC_IP`, ...). The bridge also announces itself as `ts570.local`.

Status LED: red = no WiFi, blue = WiFi up, green = client connected, white
flash = data passing.

One client at a time; a new connection replaces the old one, so a client that
reconnects always gets the radio.

## Quick test

From any PC on the same network:

```bash
printf 'FA;' | nc -w 1 ts570.local 7373
```

The answer is the VFO A frequency, e.g. `FA00014074000;`.

## Using it

- **piHPSDR-IF:** a direct "Kenwood over TCP" CAT mode is planned; it will
  connect to `<bridge IP>:7373` without rigctld.
- **With rigctld (works today):** create a virtual serial port that points at
  the bridge and give it to rigctld:

  ```bash
  socat pty,link=/tmp/ts570,raw,echo=0 tcp:ts570.local:7373 &
  rigctld -m 2004 -r /tmp/ts570 -s 57600
  ```
