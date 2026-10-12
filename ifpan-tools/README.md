# ifpan-tools — helper scripts and test programs of piHPSDR-IF

Everything here was used while building and testing piHPSDR-IF (2026-10)
and is kept so nothing gets lost. None of it is part of the program build.
Documentation: `docs/README-IFPAN.md`.

## linux-pi/ — files installed on the Raspberry Pi 5 server

| File | Installed as | Purpose |
|---|---|---|
| `make.config.pihpsdr` | `~/pihpsdr-if/make.config.pihpsdr` | build options (IFPAN, SoapySDR, MIDI, PulseAudio, no GPIO) |
| `run-pihpsdr-if.sh` | `~/pihpsdr-if/run-pihpsdr-if.sh` | launcher: starts in the source folder, log to `~/pihpsdr-if.log` |
| `pihpsdr-if.desktop` | `~/Desktop/` and `~/.local/share/applications/` | desktop / menu shortcut |
| `ts570-cat-bridge.sh` | `/usr/local/bin/` | CAT bridge: TCP 127.0.0.1:7373 <-> FTDI USB-RS232 at 57600 |
| `ts570-cat.service` | `/etc/systemd/system/` | runs the bridge (`systemctl enable --now ts570-cat`) |
| `blacklist-msi2500.conf` | `/etc/modprobe.d/` | keeps the kernel DVB drivers off the MSi2500 clone |
| `99-msi2500.rules` | `/etc/udev/rules.d/` | user access to USB 1DF7:2500 |
| `soapymiri-transfer-mode.patch` | applied to SoapyMiri before building | keeps the USB transfer mode (ISOC) as a copy (same file as `WINDOWS/installer/` in `windows-port`) |

See `docs/IFPAN-INSTALL-LINUX.md`.

## windows-wsl-cat/ — CAT bridge on the Windows PC

The FTDI USB-RS232 adapter is passed to WSL (Ubuntu-24.04) with usbipd;
socat in WSL serves it on TCP 7373, reachable from Windows at 127.0.0.1:7373.

| File | Installed as |
|---|---|
| `ts570-cat-bridge.sh` | WSL `/usr/local/bin/ts570-cat-bridge.sh` |
| `ts570-cat.service` | WSL `/etc/systemd/system/ts570-cat.service` |
| `ts570-cat-bridge.vbs` | Windows Startup folder (`shell:startup`): keeps WSL running and runs `usbipd attach --auto-attach` for 0403:6001 |

See `docs/IFPAN-INSTALL-WINDOWS.md`.

## net/ — network loss test

`udpecho.py` (on the server) echoes UDP port 9999; `udptest.py <host>
<seconds>` (on the client) sends 100 packets/s of 1400 bytes and reports
lost packets, loss bursts and round-trip times. The client's RX audio is
UDP: every lost packet is an audio gap. Found the LAN fault of 2026-10-11
(0.5–0.9 % lost, CRC errors on the Pi's Ethernet). Windows Firewall blocks
incoming ICMP, so run the sender on the PC and the echo on the Pi.

```
pi$  python3 udpecho.py &
pc>  python udptest.py 192.168.0.107 120
```

## cat/ — TS-570D CAT probes

Only with exclusive access to the radio (stop piHPSDR-IF first: the TS-570D
drops commands that arrive while it answers someone else). Most talk to the
CAT bridge at 127.0.0.1:7373.

| Script | What it checked |
|---|---|
| `ts570_serial_probe.py` | direct serial (`/dev/ttyUSB0`, 57600) command / answer |
| `ts570_ac_tuner_test.py` | `AC;` antenna tuner read / on / through |
| `ts570_pc_power_test.py` | `PC;` output power set / read back per mode (**transmits into the dummy load**) |
| `ts570_tx_meter_poll.py` | `IF;RM;SM;` polling during TX (TX flag, meters) |
| `rigctld_smeter_test.py` | S-meter through Hamlib rigctld (port 4532) |

## miritest/ — MSi2500 clone tests (Windows, MSYS2 MINGW64)

Built with `gcc x.c -lSoapySDR` (or `-lmirisdr`, `-lusb-1.0`). Copy
`libusb-1.0.dll` 1.0.30 next to the exe: an old 1.0.24 in
`C:\Windows\System32` is loaded first otherwise and has no working
isochronous support.

| Program | Purpose |
|---|---|
| `usbdesc.c` | prints the clone's USB descriptors (alt settings, isoc/bulk endpoints) |
| `usbreset.c` | `libusb_reset_device` on 1DF7:2500 |
| `miridirect.c` | libmirisdr async streaming without SoapySDR (`ISOC` / `BULK`) |
| `miritest.c` | SoapySDR streaming, open / close cycles (found: WinUSB fails, libusbK + ISOC works) |
| `ptttest.c`, `ptt5.c` | streaming while the Kenwood is keyed by CAT (`TX;`/`RX;`), measures stalls and the fastest recovery (found the ground-loop stall at 25 W, worklog 4.12) — **keys the transmitter** |

## windows/

`capwin.ps1 [-match <title part>] [-out <png>]`: DPI-aware capture of a
piHPSDR-IF window (used to check menu layouts).
