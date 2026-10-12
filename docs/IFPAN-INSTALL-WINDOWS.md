# piHPSDR-IF on Windows — install, set up, build

Part A is for **using** piHPSDR-IF on a Windows PC (server next to the
radio, or client of a Raspberry Pi server). Part B is for **building** the
MSI installer. Code: branch **`windows-port`** of
https://github.com/jacintomfr/pihpsdr (`cu2ed` merged into it after every
change). Older background notes: `WINDOWS/BUILD.md` and
`WINDOWS/README-Windows.txt` in that branch (written in the RTL-SDR /
Pluto / rigctld days; this file supersedes them where they differ).

See `docs/README-IFPAN.md` for what piHPSDR-IF is and
`docs/IFPAN-USER-GUIDE.md` for how to use it.

---

# Part A — install and set up

## A1. Install the MSI

Run `piHPSDR-IF-3.1.N-x64.msi` (administrator rights). Installs per machine
to `C:\Program Files\piHPSDR-IF`; Start Menu folder "piHPSDR-IF" (program,
"Start rigctld (TS-570D)", read-me, settings folder), optional desktop
shortcut. A newer MSI upgrades in place.

Quiet install / upgrade (what is used during development):

```bat
taskkill /IM pihpsdr-if.exe
msiexec /i piHPSDR-IF-3.1.N-x64.msi /passive /norestart
"C:\Program Files\piHPSDR-IF\bin\pihpsdr-if.exe"
```

* **Settings and logs:** `%LOCALAPPDATA%\piHPSDR-IF` — `soapyMiri.props`
  (one props file per radio: `rtlsdr.props`, `plutosdr.props`),
  `remote.props`, `protocols.props`, `pihpsdr.stdout` (the log),
  `pihpsdr.stderr`, `wdspWisdom01`. Kept on uninstall.
* **First start:** WDSP computes its FFT wisdom (a few minutes, progress in
  the start window). Only once.
* **Version:** 3.1.N, N = commits since the v3.1 tag. **Server and client
  must run the same build** (own client/server protocol version).
* Bundled: GTK3 runtime, WDSP, SoapySDR with the modules **soapyMiri**
  (libmirisdr-5), **rtlsdr**, **plutosdr**, libusb 1.0.30, RADE (Opus +
  neural weights), RNNoise, rigctld (Hamlib 4.5.5), `SoapySDRUtil.exe`,
  `rtl_test.exe`, `iio_info.exe`.

## A2. SDR driver (Zadig)

**MSi2500 / SDRplay RSP1A clone (USB 1DF7:2500)** — needs **libusbK**:

1. Zadig (zadig.akeo.ie) → Options → List All Devices → the device with USB
   ID **1DF7 2500** → driver **libusbK** → Replace Driver. Do not change
   any other device.
2. Unplug and plug the clone again.
3. Check (command prompt in `C:\Program Files\piHPSDR-IF\bin`):
   `SoapySDRUtil --find="driver=soapyMiri"`.

Found on 2026-10-11: with **WinUSB** the clone never streams (bulk:
timeouts only; isochronous: libusb crashes). libusbK + isochronous
transfers (piHPSDR-IF asks for them) stream 1.536 MS/s and reopen without
replugging. Use a USB3 port, no hub.

**RTL-SDR (0BDA:2838)** — needs **WinUSB**: Zadig → List All Devices →
"Bulk-In, Interface (Interface 0)" 0BDA 2838 → WinUSB → Replace Driver.
Check: `rtl_test`, `SoapySDRUtil --find="driver=rtlsdr"`.

**PlutoSDR** — Analog Devices "PlutoSDR-M2k-USB-Drivers" for a USB Pluto;
opened as `uri=ip:192.168.2.1`.

> Test programs of your own (not the MSI): an old `libusb-1.0.dll` 1.0.24
> in `C:\Windows\System32` (installed by another program) is loaded before
> the 1.0.30 the clone needs. Copy 1.0.30 next to the test exe. The MSI is
> not affected.

## A3. CAT link to the TS-570D

TS-570D: **menu 35 = "576-1" (57600 baud)**, then switch the radio off and
on. piHPSDR-IF: Menu → Radio → "CAT" = **Kenwood TCP**, address
**127.0.0.1:7373** (or rigctld, see below).

### Option 1 (used on this PC): FTDI adapter → WSL → socat → TCP 7373

The FTDI USB-RS232 adapter (0403:6001) is handed to WSL (Ubuntu-24.04) with
usbipd, and socat in WSL serves the serial port on TCP 7373. Files in
`ifpan-tools/windows-wsl-cat/`.

1. Windows (administrator), once:

   ```bat
   winget install usbipd
   usbipd list                                 :: find the FTDI adapter (0403:6001)
   usbipd bind --hardware-id 0403:6001
   ```

2. WSL Ubuntu-24.04, once: systemd on (`/etc/wsl.conf`:
   `[boot]` / `systemd=true`, then `wsl --shutdown`), and

   ```bash
   sudo apt install -y socat
   sudo cp ts570-cat-bridge.sh /usr/local/bin/ && sudo chmod +x /usr/local/bin/ts570-cat-bridge.sh
   sudo cp ts570-cat.service /etc/systemd/system/
   sudo systemctl daemon-reload && sudo systemctl enable --now ts570-cat.service
   ```

   `ts570-cat-bridge.sh` waits for `/dev/ttyUSB0`, then runs
   `socat TCP4-LISTEN:7373,reuseaddr,fork FILE:/dev/ttyUSB0,b57600,raw,echo=0`
   (IPv4, so Windows reaches it at 127.0.0.1:7373, which does not change
   when WSL restarts).

3. Windows Startup folder (`Win+R` → `shell:startup`): copy
   `ts570-cat-bridge.vbs`. At logon, hidden, it keeps the distro running
   (`wsl.exe -d Ubuntu-24.04 -e sleep infinity`) and runs
   `usbipd attach --wsl --hardware-id 0403:6001 --auto-attach`, which
   re-attaches the adapter whenever it is replugged.

4. Check: `printf 'FA;' | socat - TCP:127.0.0.1:7373` in WSL →
   `FA00014162000;`.

If the adapter re-enumerates (seen once), CAT comes back by itself; if not:
`wsl -d Ubuntu-24.04 -u root systemctl restart ts570-cat`.

### Option 2: Hamlib rigctld

Start Menu "Start rigctld (TS-570D)" (asks for the COM port) or
`rigctld -m 2004 -r COM3 -s 57600` in the `bin` folder (`\\.\COM10` from
COM10 up). piHPSDR-IF: "CAT" = rigctld, 127.0.0.1:4532.

### Option 3: ESP32 WiFi bridge

`ESP32/ts570_wifi_bridge` (TCP 7373, needs a 3.3 V MAX3232; compiled, not
yet tried on hardware). The FTDI adapter put interference lines on the
panadapter; the WiFi bridge was meant to avoid the cable.

## A4. Audio

PortAudio; every device appears once per Windows audio system ("MME: ...",
"Windows DirectSound: ...", "Windows WASAPI: ..." — WASAPI only when the
device is set to 48 kHz in Windows). MME works with any rate.

* **TX audio to the Kenwood** (server): Menu → Radio → "TX audio to TS" =
  the sound card output wired through a 600 Ω isolation transformer into
  the TS-570D **ACC2**. Level: that output's volume (ALC barely moving).
* **Speaker / microphone**: RX menu (output), TX menu (microphone), as in
  piHPSDR. On a client, the microphone is sent to the server.

## A5. Server and client

* **Server** (the machine with the SDR, the CAT link and the ACC2 sound
  card): Menu → Server → "Server Enable", port 50000. "Server runs
  protocol only if client is connected" stops the SDR while no client is
  connected (the server's own screen then stays black).
* **Client**: Discovery window → "Use Server" → `host:50000` (a Pi server:
  `192.168.0.107:50000`) → Start. Same MSI version as the server.
* Both on one PC (as during development): client to `127.0.0.1:50000`.
* **From outside home:** install Tailscale on the client PC (same account as
  the Pi) and connect to the Pi's Tailscale address, `100.88.105.1:50000`
  (Linux guide, section 9). The MSI is plain x86-64 (no AVX / AVX2: 0 ymm
  instructions in `pihpsdr-if.exe`, RADE / Opus included), so older PCs run it.
* The client's RX audio is UDP: the LAN must not lose packets (half-second
  audio cuts = packet loss; test with `ifpan-tools/net/`, see the Linux
  guide section 10). Check the PC's link speed (Settings → Network, or
  `Get-NetAdapter`): a Gigabit card at 100 Mbps points to a faulty cable.

## A6. Known PC-specific problem: ground loop with the MSi2500 clone

With the **Windows PC as server**, transmitting with ≥ 25 W (AM into the
dummy load, or SSB with speech) knocked the MSi2500 clone off the USB bus
every time (5 W never). Cause: the TS-570D's supply current shifting its
ground against the PC's, through the IF coax shield and the clone to the
USB ground — touching the coax shield to the clone's ground alone was
enough. Ferrites, shorter cables and a 10 dB attenuator did not help.
piHPSDR-IF reopens the SDR ~2 s after the Kenwood is back in RX.
**With the Raspberry Pi 5 as server the problem is gone** (100 W SSB, no
stall). Hardware remedies if the PC must be the server: RF isolation
transformer or DC block in centre *and* shield on the IF line, a heavy
ground strap TS ↔ PC, check how the IF buffer is powered.

## A7. Troubleshooting

| Symptom | What to check |
|---|---|
| Window appears for 2 s, then minimised and cannot be restored (old builds) | Fixed (worklog 4.11: an off-screen position had been saved); delete the `WindowPositionX/Y` lines from the props if it ever recurs. |
| No samples from the clone | Driver must be libusbK (A2); replug after changing it. |
| `Kenwood not answering` | WSL running? `usbipd list` shows the FTDI "Attached"? Radio at 57600? |
| TX spectrum / mic / ALC dead after an SDR stall during MOX (PC server) | Seen with the ground-loop stall; restart the program. Diagnostics: `mic diag:` lines in `pihpsdr.stdout` (every 5 s). Not seen with the Pi server. |
| Client audio cuts | Packet loss on the LAN (A5); `audio diag:` lines in the log (3.1.177+). |

---

# Part B — build the MSI

## B1. Build PC prerequisites

MSYS2 in `C:\msys64`, **MINGW64** environment (not UCRT64). Always build
from a real MSYS2 MINGW64 shell, or from PowerShell / Git Bash with:

```powershell
$env:MSYSTEM = 'MINGW64'; $env:CHERE_INVOKING = '1'
C:\msys64\usr\bin\bash.exe -lc "cd /c/Users/jacin/Documents/pihpsdr-cu2ed-win && WINDOWS/installer/build-exes.sh"
```

> **Trap:** Git for Windows' bash finds another gcc first (e.g.
> `C:\ProgramData\mingw64`, gcc 15.2) and the link fails with
> `undefined reference to '_gnu_exception_handler'` (seen 2026-10-11); or
> gcc fails with "Cannot create temporary file in C:\Windows\". Use the
> MSYS2 bash as above (`which cc` must be `/mingw64/bin/cc`).

Packages:

```bash
pacman -S --needed git make patch curl \
  mingw-w64-x86_64-gcc mingw-w64-x86_64-pkgconf mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-gtk3 mingw-w64-x86_64-fftw mingw-w64-x86_64-portaudio \
  mingw-w64-x86_64-opus mingw-w64-x86_64-curl mingw-w64-x86_64-sqlite3 \
  mingw-w64-x86_64-openssl mingw-w64-x86_64-libwebsockets mingw-w64-x86_64-miniupnpc \
  mingw-w64-x86_64-libiio mingw-w64-x86_64-libad9361-iio mingw-w64-x86_64-libusb \
  mingw-w64-x86_64-speexdsp mingw-w64-x86_64-boost \
  autoconf automake libtool gettext-devel
```

**SoapySDR** has no MSYS2 package; build it once into `/mingw64`:

```bash
cd /tmp && git clone https://github.com/pothosware/SoapySDR.git
cmake -S SoapySDR -B SoapySDR/build -G "MSYS Makefiles" -DCMAKE_INSTALL_PREFIX=/mingw64 -DCMAKE_BUILD_TYPE=Release
cmake --build SoapySDR/build -j"$(nproc)" && cmake --install SoapySDR/build
pkg-config --modversion soapysdr
```

**WiX Toolset v3.x** (`C:\Program Files (x86)\WiX Toolset v3.14\bin`, or
set `WIX_BIN`).

## B2. Run-time helpers (once): `WINDOWS/installer/build-deps.sh`

Builds into `/mingw64` (sources in `build-win/deps`, git-ignored; re-runs
skip what exists):

* **SoapyPlutoSDR**;
* **librtlsdr** (osmocom; `-std=gnu17`, its K&R `getopt.c` fails as C23) +
  **SoapyRTLSDR**;
* **libmirisdr-5** (library only: its two CLI tools use POSIX signals) +
  **SoapyMiri** with `WINDOWS/installer/soapymiri-transfer-mode.patch`;
* **Hamlib 4.5.5** `rigctld` (static).

## B3. RADE libraries (once)

Either copy deskHPSDR's Windows `rade-libs/` (same toolchain) into the
source folder, or build: `RADE_C_DIR=/path/to/rade_c ./build-rade.sh`
(`rade_c` commit c394514, private repository; network for Opus and its
model). Without `rade-libs/` the program builds without RADE.
`docs/RADE-IFPAN.md` section 6.

## B4. Build

In `pihpsdr-cu2ed-win` (worktree of the same repository on branch
`windows-port`):

```bash
git merge --no-edit cu2ed                  # bring in the shared changes
WINDOWS/installer/build-exes.sh            # make clean + make -> build-win/exe/pihpsdr-if.exe
WINDOWS/installer/stage.sh                 # self-contained folder build-win/stage (DLL closure from the PE imports)
WINDOWS/installer/build-msi.sh             # -> build-win/piHPSDR-IF-3.1.N-x64.msi
```

* **Never tag**: the version comes from `git describe` (3.1.N).
* Never build the Linux version in this checkout (Linux `.o` / `.a` files
  would be linked silently into the exe); `build-exes.sh` starts with
  `make clean`. Linux check builds go to a separate tree (WSL
  `~/pihpsdr-cu2ed`).
* `strip --remove-section=.comment` on the exe (in `build-exes.sh`):
  otherwise the PE section order is invalid and other PCs refuse the exe
  (error 193).
* Text is drawn through fontconfig/FreeType (`WINDOWS/compat/cairo.h`,
  `win_fonts.c`): MSYS2's cairo aborts in its DirectWrite toy-font path.
* `stage.sh` ships `libsoapyMiriSupport.dll`, `librtlsdrSupport.dll`,
  `libPlutoSDRSupport.dll`; their libraries come through the DLL closure.

## B5. Windows-specific code

`WINDOWS/compat/` (POSIX shims: sockets, termios/COM, fcntl, pwd, endian,
pthread time functions, cairo fonts), a `WINDOWS` section in the Makefile
(`GPIO=OFF MIDI=OFF USBOZY=OFF AUDIO=PORTAUDIO`), small `_WIN32` blocks in
`src/` (settings folder, shutdown, COM default, PortAudio device names).
Fixed on the way (worklog 4.1 / 4.6): MinGW `clock_nanosleep` with
`TIMER_ABSTIME` returned at once (affected the TX audio and CW threads);
Windows `gettimeofday` 15.6 ms steps (client latency).

Note: **MIDI is off on Windows** (`MIDI=OFF`); a MIDI VFO works on the
Linux / Pi build.
