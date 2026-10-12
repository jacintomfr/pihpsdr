# piHPSDR-IF on Linux / Raspberry Pi — install from scratch

Complete recipe for a Linux machine, written from the Raspberry Pi 5 server
that was set up on 2026-10-11/12 (hostname SDR-IQ, Debian 13 "trixie"
aarch64, Raspberry Pi OS desktop with labwc/Wayland and PipeWire). The same
steps work on a Raspberry Pi 4 and on Debian/Ubuntu x86-64 PCs (package
names may differ slightly).

Branch: **`cu2ed`** of https://github.com/jacintomfr/pihpsdr (Linux and the
shared code). See `docs/README-IFPAN.md` for what piHPSDR-IF is and
`docs/IFPAN-USER-GUIDE.md` for how to use it.

Contents:
1. Packages
2. SDR: MSi2500 / SDRplay RSP1A clone (SoapyMiri), RTL-SDR, PlutoSDR
3. RADE libraries
4. Build piHPSDR-IF
5. First start, settings folder, launcher
6. CAT link to the TS-570D (socat bridge service)
7. Audio (TX audio to ACC2, local speaker / microphone)
8. MIDI VFO / controller
9. Server for remote clients
10. Checks and troubleshooting
11. What is where (summary)

---

## 1. Packages

piHPSDR's own dependency script is `LINUX/libinstall.sh` (it also builds
SoapySDR from source). On the Pi 5 the Debian packages were used instead;
this list is enough for a piHPSDR-IF build with SoapySDR, MIDI, TCI, RADE:

```bash
sudo apt update
sudo apt install -y build-essential git pkg-config cmake autoconf automake \
  autopoint gettext libtool dos2unix curl \
  libfftw3-dev libgtk-3-dev libasound2-dev libssl-dev libcurl4-openssl-dev \
  libusb-1.0-0-dev libpulse-dev libpipewire-0.3-dev libopus-dev \
  libminiupnpc-dev libsqlite3-dev libwebsockets-dev zlib1g-dev \
  libspeexdsp-dev \
  libsoapysdr-dev soapysdr-tools \
  socat \
  fonts-open-sans fonts-freefont-otf fonts-roboto
```

(`libgpiod-dev` / `libi2c-dev` are only needed with `GPIO=ON`, which
piHPSDR-IF does not use.)

> **Trap — Debian's old Mirics module.** `soapysdr-tools` may pull the
> meta package `soapysdr0.8-module-all`, which brings
> `soapysdr0.8-module-mirisdr` (SoapySDR driver **"miri"**, based on the old
> libmirisdr-4). It finds the MSi2500 clone first; piHPSDR then opens it
> with the wrong driver at 768 kS/s. Remove it (only that module; the meta
> package goes with it, the other modules stay):
>
> ```bash
> sudo apt remove -y soapysdr0.8-module-mirisdr
> SoapySDRUtil --info      # "Available factories" must show soapyMiri, not miri
> ```

## 2. SDR on the 73.05 MHz IF

### 2.1 MSi2500 / SDRplay RSP1A clone (recommended, used on the Pi 5)

USB ID **1DF7:2500** (MSi2500 USB bridge + MSi001 tuner). Driven by
**libmirisdr-5 + SoapyMiri** (both GPL/open; the closed SDRplay API is not
used and not needed).

**a) Keep the kernel's DVB drivers away from it** (otherwise they claim the
device):

```bash
printf 'blacklist msi2500\nblacklist msi001\nblacklist sdr_msi3101\n' | \
  sudo tee /etc/modprobe.d/blacklist-msi2500.conf
echo 'SUBSYSTEM=="usb", ATTRS{idVendor}=="1df7", ATTRS{idProduct}=="2500", MODE="0666"' | \
  sudo tee /etc/udev/rules.d/99-msi2500.rules
sudo udevadm control --reload-rules
sudo rmmod msi2500 msi001 2>/dev/null; true
```

Check: `lsusb -t` shows the device with `Driver=[none]` (or `usbfs` while
piHPSDR-IF has it open).

**b) Build libmirisdr-5 and SoapyMiri** (sources in `~/deps`):

```bash
mkdir -p ~/deps && cd ~/deps
git clone https://github.com/ericek111/libmirisdr-5.git
git clone https://github.com/ericek111/SoapyMiri.git        # tested at commit 1e0a125

cmake -S libmirisdr-5 -B libmirisdr-5/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build libmirisdr-5/build -j3
sudo cmake --install libmirisdr-5/build && sudo ldconfig

# SoapyMiri fix (ifpan-tools/linux-pi/soapymiri-transfer-mode.patch in this
# repository): it kept the USB transfer mode as a pointer into setupStream's
# arguments, which are gone when activateStream reads it again.
cp ~/pihpsdr-if/ifpan-tools/linux-pi/soapymiri-transfer-mode.patch ~/
sed -i 's/\r$//' ~/soapymiri-transfer-mode.patch     # needed if the file came from Windows (CRLF)
git -C SoapyMiri apply ~/soapymiri-transfer-mode.patch

cmake -S SoapyMiri -B SoapyMiri/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build SoapyMiri/build -j3
sudo cmake --install SoapyMiri/build
```

(Clone piHPSDR-IF first, section 4a, or fetch the patch alone from
GitHub. The same patch is used by the Windows build:
`WINDOWS/installer/soapymiri-transfer-mode.patch` in `windows-port`.)
All files installed on the Pi 5 are also in `ifpan-tools/linux-pi/`.

**c) Check:**

```bash
SoapySDRUtil --find="driver=soapyMiri"          # Mirics MSi2500 ... driver = soapyMiri
SoapySDRUtil --args="driver=soapyMiri" --rate=1.536e6 --direction=RX   # ~1.53 Msps (Ctrl-C)
```

`SoapySDRUtil` uses bulk transfers and shows some "samples lost"; piHPSDR-IF
asks for **isochronous** transfers (`transfer=ISOC`, log line
`SoapyMiri Using ISOC transfer mode`) at **1.536 MS/s**.

**d) USB port.** Plug the clone **straight into a Pi USB port** (the blue
USB3 ones), not into a hub. Measured on the Pi 5: behind a USB 2.0 hub
shared with the CAT adapter and a MIDI/audio device, ~3.5 "504 samples lost"
messages per second; on a direct port ~8 per minute.

### 2.2 RTL-SDR (R820T, ideally with TCXO)

```bash
sudo apt install -y soapysdr-module-rtlsdr      # or soapysdr0.8-module-rtlsdr
SoapySDRUtil --find="driver=rtlsdr"
```

Blacklist the DVB driver if needed (`dvb_usb_rtl28xxu`). Runs at 1.536 MS/s;
use the RF slider for the gain. Frequency calibration: Radio menu.

### 2.3 PlutoSDR

`soapysdr-module-plutosdr` (or `LINUX/soapy.pluto.sh`). Opened as
`uri=ip:<host>` (default 192.168.2.1). At 73 MHz the RTL-SDR and the
MSi2500 receive better than the Pluto.

## 3. RADE libraries (FreeDV RADE V1)

RADE is built in automatically when `rade-libs/lib/librade_static.a` exists
in the source tree. Needs the `rade_c` sources (private repository
`jacintomfr/rade_c`, branch `hpsdr-rs-local`, commit **c394514**; the same
commit is in `hpsdr-rs-git/vendor/rade_c`) and network access (Opus and its
neural model, about 180 MB):

```bash
cd ~/pihpsdr-if
RADE_C_DIR=$HOME/rade_c ./build-rade.sh > ~/build-rade.log 2>&1
tail -3 ~/build-rade.log                    # "RADE libraries installed in .../rade-libs"
```

On the Pi 5 the model download (~200 KB/s here) took most of the ~20 minutes;
compiling is quick. `rade-libs/`, `rade-src/`, `rade-build/` are git-ignored.
Details: `docs/RADE-IFPAN.md`.

## 4. Build piHPSDR-IF

**a) Sources.** Either clone:

```bash
git clone -b cu2ed https://github.com/jacintomfr/pihpsdr.git ~/pihpsdr-if
```

or copy an export from another machine. If the files come from Windows,
convert line ends first (a CRLF in `rnnoise/model_version` breaks the RNNoise
model download URL, a CRLF patch does not apply):

```bash
cd ~/pihpsdr-if
grep -rlI $'\r' --exclude-dir=.git --exclude-dir=rade-libs . | xargs -r sed -i 's/\r$//'
```

**b) Build options** — file `make.config.pihpsdr` in the source folder:

```
GPIO=OFF
MIDI=ON
USBOZY=OFF
SOAPYSDR=ON
IFPAN=ON
PORTFORWARD=ON
TCI=ON
AUDIO=PULSE
```

(`IFPAN=ON` makes the binary `pihpsdr-if` and requires `SOAPYSDR=ON`.
`AUDIO=PULSE` works with PipeWire's pulse server.)

**c) Build:**

```bash
cd ~/pihpsdr-if
make -j4 > ~/build-if.log 2>&1; echo $?          # 0 = OK
grep -E " error|warning: " ~/build-if.log
ls -la pihpsdr-if
```

The first build downloads the RNNoise model (`rnnoise/src/rnnoise_data.c`,
~78 MB, by `rnnoise/model_version`). With RADE the binary is ~53 MB.

**Updating later:** `git pull` (or copy the changed `src/` files) and
`make -j4` again.

## 5. First start, settings folder, launcher

Start it from the source folder (it then uses that folder as its working
and settings folder, like piHPSDR):

```bash
cd ~/pihpsdr-if && ./pihpsdr-if
```

* **First start:** WDSP computes its FFT "wisdom" (a few minutes on a Pi,
  progress in the start window; `Planning COMPLEX ... FFT size ...` in the
  log). Only once.
* **Settings:** `~/pihpsdr-if/soapyMiri.props` (one props file per radio:
  `rtlsdr.props`, `plutosdr.props`, ...), `remote.props`,
  `protocols.props`. When started elsewhere the settings go to
  `~/.config/pihpsdr-if`.
* **Copying settings from the Windows PC** works: the props files are
  plain text (`%LOCALAPPDATA%\piHPSDR-IF\soapyMiri.props`). Clear the
  Windows sound device names afterwards (they do not exist on Linux):
  `receiver.0.audio_name=`, `receiver.0.local_audio=0`,
  `transmitter.8.audio_name=`, `transmitter.8.local_audio=0`,
  `ifpan.tx_out=` — then choose the Linux devices in the menus. Also check
  the Server menu option "Server runs protocol only if client is
  connected" (see section 9).

**Launcher** (what is installed on the Pi 5):

`~/pihpsdr-if/run-pihpsdr-if.sh`:

```sh
#!/bin/sh
cd "$HOME/pihpsdr-if" && exec ./pihpsdr-if > "$HOME/pihpsdr-if.log" 2>&1
```

`~/Desktop/pihpsdr-if.desktop` (and a copy without the first line in
`~/.local/share/applications/` for the menu):

```ini
#!/usr/bin/env xdg-open
[Desktop Entry]
Version=1.0
Type=Application
Terminal=false
Name=piHPSDR-IF
Comment=TS-570D IF panadapter (SoapyMiri)
Exec=/home/pi/pihpsdr-if/run-pihpsdr-if.sh
Icon=/home/pi/pihpsdr-if/piHPSDR-IF_logo.png
```

```bash
chmod +x ~/pihpsdr-if/run-pihpsdr-if.sh ~/Desktop/pihpsdr-if.desktop
cp ~/pihpsdr-if/release/pihpsdr/piHPSDR_logo.png ~/pihpsdr-if/piHPSDR-IF_logo.png
```

On this Pi the desktop icons sit in boxes drawn into the wallpaper
(`~/.config/pcmanfm/default/wall_boxes.png`, positions in
`desktop-items-HDMI-A-1.conf`; piHPSDR-IF at x=776 y=142 under deskHPSDR,
box added to the wallpaper, original kept as
`wall_boxes.png.before-pihpsdr-if`). Reload the desktop with
`~/refresh_desktop.sh` (restarts `pcmanfm --desktop`).

Log: `~/pihpsdr-if.log`.

## 6. CAT link to the TS-570D

The TS-570D's RS-232 port is reached through a USB-RS232 adapter (FTDI
FT232, 0403:6001). **TS-570D menu 35 = "576-1" (57600 baud)**, then switch
the radio off and on.

piHPSDR-IF talks Kenwood CAT over TCP ("CAT via Kenwood TCP", address
`127.0.0.1:7373`); a small socat service bridges TCP to the serial port. It
waits for the adapter and comes back by itself when the adapter is
re-plugged:

`/usr/local/bin/ts570-cat-bridge.sh`:

```sh
#!/bin/sh
# TS-570D CAT bridge: TCP 127.0.0.1:7373 <-> FTDI USB-RS232 (57600 8N1)
while true; do
  DEV=$(ls /dev/serial/by-id/*FTDI* 2>/dev/null | head -1)
  if [ -n "$DEV" ]; then
    stty -F "$DEV" 57600 raw -echo 2>/dev/null
    socat TCP-LISTEN:7373,bind=127.0.0.1,reuseaddr,fork FILE:"$DEV",b57600,raw,echo=0
  fi
  sleep 2
done
```

`/etc/systemd/system/ts570-cat.service`:

```ini
[Unit]
Description=TS-570D CAT bridge (TCP 127.0.0.1:7373 <-> FTDI) for piHPSDR-IF
After=network.target
[Service]
ExecStart=/usr/local/bin/ts570-cat-bridge.sh
Restart=always
RestartSec=2
[Install]
WantedBy=multi-user.target
```

```bash
sudo chmod +x /usr/local/bin/ts570-cat-bridge.sh
sudo systemctl daemon-reload && sudo systemctl enable --now ts570-cat.service
printf 'FA;' | timeout 2 socat - TCP:127.0.0.1:7373; echo     # -> FA00014162000;
```

(The device is found by its `/dev/serial/by-id/*FTDI*` name, so other
serial devices — e.g. an Arduino MIDI controller — do not get in the way.
`bind=127.0.0.1` keeps the radio's CAT off the network.)

In piHPSDR-IF: Menu → Radio → "CAT" = **Kenwood TCP**, address
**127.0.0.1:7373**. Alternatives: Hamlib `rigctld -m 2004 -r /dev/ttyUSB0 -s
57600` with "CAT" = rigctld (127.0.0.1:4532), or the ESP32 WiFi bridge
(`ESP32/ts570_wifi_bridge`, TCP 7373).

## 7. Audio

* **TX audio to the Kenwood:** a sound card output wired through a 600 Ω
  isolation transformer into the TS-570D **ACC2** audio input. Menu →
  Radio → "TX audio to TS" (server only). Level: sound card output volume,
  set so the Kenwood's ALC barely moves.
* **Local speaker / microphone:** Menu → RX (output) and TX (microphone),
  as in piHPSDR. With PipeWire the devices appear as pulse sinks/sources;
  if a device is missing, `systemctl --user restart wireplumber`.

## 8. MIDI VFO / controller

`MIDI=ON` in the build. A USB MIDI controller (here the Arduino Due MIDI
controller with a 600 PPR optical encoder, project
`jacintomfr/hpsdr-midi-optical`) is set up in Menu → MIDI, as in piHPSDR:
assign the encoder to the VFO. With "Clicks tune the TS" on, every step is
sent to the Kenwood and the display slides at once (user guide, "Tuning").
This is the way of working for a remote station: nobody touches the TS
dial.

## 9. Server for remote clients

Menu → Server: "Server Enable", port **50000** (`radio.hpsdr_server.listen_port`).
The client (Windows or Linux piHPSDR-IF of the **same build**) connects from
its Discovery window ("Use Server", host:50000).

* **"Server runs protocol only if client is connected"**: when on, the
  server stops the SDR while no client is connected — the Pi's own screen
  then shows a **black spectrum and waterfall**. Switch it off to use the
  Pi's screen without a client.
* The RX audio to the client is UDP without retransmission: the network
  must be clean (see 10).

## 10. Checks and troubleshooting

| Symptom | Cause / fix |
|---|---|
| Discovery shows the clone, but the log says `Driver=miri` and 768000 | Debian's `soapysdr0.8-module-mirisdr` is installed: remove it (section 1). |
| Black spectrum / waterfall on the Pi, log: `soapy_receive_single_thread: deactivateStream` right after the start | Server option "Server runs protocol only if client is connected" (section 9). |
| Many `504 samples lost, 3072, ...` lines in the log | The clone is behind a hub or on a busy USB bus: use a direct Pi port. A few per minute are normal. |
| `Kenwood not answering` | CAT service: `systemctl status ts570-cat`, adapter in `/dev/serial/by-id/`, radio at 57600 (menu 35). `printf 'FA;' \| socat - TCP:127.0.0.1:7373`. |
| Client: ~0.5 s audio cuts now and then | Packet loss on the LAN. Pi: `sudo ethtool -S eth0 \| grep -E "frame_check\|symbol"` (CRC errors = cable / switch port / interference); PC: link speed (a Gigabit card at 100 Mbps = faulty cable pair). Direct cable or another switch port. UDP test below. |
| `make`: `curl: (3) URL rejected` (rnnoise) | CRLF in `rnnoise/model_version`: convert the sources (section 4a). |
| SoapyMiri patch does not apply | CRLF in the patch file: `sed -i 's/\r$//'` it. |

**UDP loss test** (what found the LAN problem): on the Pi run an echo
server, on the PC send 100 packets/s of 1400 bytes and count what comes
back (a lost packet = an audio gap at the client):

```python
# Pi: ~/udpecho.py
import socket
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(("0.0.0.0",9999))
while True:
    d,a=s.recvfrom(2048); s.sendto(d,a)
```

Both scripts are in `ifpan-tools/net/` (`python3 udpecho.py &` on the Pi,
`python udptest.py <pi-address> 120` on the PC).
Clean result: 0 lost. Found on 2026-10-11: 0.5–0.9 % lost, matching the
Pi's CRC error counter — a physical-layer fault.

**Audio diagnostics** in the log (since 3.1.177): `audio diag: server RX
audio send gap N ms` (server stalled), `client RX audio arrival gap N ms`
(network), `RX1 output buffer ran dry / overrun` (client sound card).

**CPU** (Pi 5, RX only, 1.536 MS/s, no RADE): ~93 % of one core in total
(~23 % of the four cores), SoC ~60 °C.

## 11. What is where (Pi 5, summary)

| Item | Location |
|---|---|
| Sources + binary + settings | `~/pihpsdr-if` (`pihpsdr-if`, `soapyMiri.props`, `make.config.pihpsdr`, `rade-libs/`) |
| Log | `~/pihpsdr-if.log` (launcher), build logs `~/build-if.log`, `~/build-rade.log` |
| RADE sources | `~/rade_c` |
| libmirisdr-5 / SoapyMiri sources | `~/deps` (installed to `/usr/local/lib`, `/usr/lib/aarch64-linux-gnu/SoapySDR/modules0.8/libsoapyMiriSupport.so`) |
| Kernel blacklist / udev | `/etc/modprobe.d/blacklist-msi2500.conf`, `/etc/udev/rules.d/99-msi2500.rules` |
| CAT bridge | `/usr/local/bin/ts570-cat-bridge.sh`, `ts570-cat.service` |
| Launcher | `~/pihpsdr-if/run-pihpsdr-if.sh`, `~/Desktop/pihpsdr-if.desktop`, `~/.local/share/applications/pihpsdr-if.desktop` |
| Removed package | `soapysdr0.8-module-mirisdr` (must stay removed) |
