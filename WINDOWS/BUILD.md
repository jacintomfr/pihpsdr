# piHPSDR-IF for Windows: build, install, run

This branch (`windows-port`) builds piHPSDR-IF (piHPSDR with `IFPAN=ON`:
receive-only IF panadapter for a Kenwood TS-570D with an ADALM-Pluto via
SoapySDR, CAT through Hamlib rigctld) natively for Windows and packages it
as an MSI. It follows the deskHPSDR Windows port (MSYS2 MinGW64 + POSIX
shim headers + WiX v3), see `C:\Users\jacin\Documents\deskhpsdr\WINDOWS\BUILD.md`
for the background of the pitfalls repeated briefly below.

## 1. What is involved

| Piece | Purpose |
|---|---|
| `WINDOWS/compat/` | POSIX compatibility shims (sockets, `getifaddrs`, `termios`/COM ports, `fcntl`, `pwd`, `endian`, cairo/pango font workaround, a `wdsp.h` wrapper) taken from the deskHPSDR port. `-I./WINDOWS/compat` comes first on the include path, so almost no `#ifdef` is needed in `src/`. |
| `Makefile` | A `WINDOWS` section keyed on `uname -s` = `MINGW64*`: forces `GPIO=OFF MIDI=OFF USBOZY=OFF AUDIO=PORTAUDIO`, adds the compat layer, Windows libraries, the icon resource and `pihpsdr-if.exe` as target. Linux/macOS builds are unchanged. |
| `wdsp/Makefile`, `wdsp/wisdom.c` | WDSP builds its native `_WIN32` code path; two warnings disabled and `WDSP_NO_CONSOLE` (no console window during the first-start FFTW planning; piHPSDR shows the progress itself). |
| `src/` | Only small `_WIN32` blocks: settings folder `%LOCALAPPDATA%\piHPSDR-IF` (startup.c), `shutdown /s` / `/r` and `COMn` serial default (radio.c), PortAudio device names prefixed with the host API (portaudio.c), `environ` declaration (client_thread.c). |
| `WINDOWS/installer/` | `build-deps.sh` (SoapyPlutoSDR, librtlsdr + SoapyRTLSDR, Hamlib rigctld), `build-exes.sh`, `stage.sh`, `build-msi.sh`, `pihpsdr-if.wxs`, icon, `start-rigctld-TS570D.bat`. |
| `WINDOWS/README-Windows.txt` | Installed next to the program (Start Menu "read me"). |

## 2. Prerequisites (once per build PC)

MSYS2 in `C:\msys64`, **MINGW64** environment (not UCRT64):

```bash
pacman -S --needed git make \
  mingw-w64-x86_64-gcc mingw-w64-x86_64-pkgconf mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-gtk3 mingw-w64-x86_64-fftw mingw-w64-x86_64-portaudio \
  mingw-w64-x86_64-opus mingw-w64-x86_64-curl mingw-w64-x86_64-sqlite3 \
  mingw-w64-x86_64-openssl mingw-w64-x86_64-libwebsockets mingw-w64-x86_64-miniupnpc \
  mingw-w64-x86_64-libiio mingw-w64-x86_64-libad9361-iio \
  autoconf automake libtool          # for Hamlib
```

SoapySDR itself has no MSYS2 package; on this PC it was already built into
`/mingw64` for deskHPSDR (recipe in deskHPSDR's `WINDOWS/BUILD.md`, section 2).

WiX Toolset v3.x (`C:\Program Files (x86)\WiX Toolset v3.14\bin`, or set `WIX_BIN`).

## 3. Building

Always from a real MSYS2 MINGW64 shell (Start Menu "MSYS2 MINGW64"), or from
PowerShell:

```powershell
$env:MSYSTEM = 'MINGW64'; $env:CHERE_INVOKING = '1'
C:\msys64\usr\bin\bash.exe -lc "cd /c/Users/jacin/Documents/pihpsdr-cu2ed-win && WINDOWS/installer/build-exes.sh"
```

(Git for Windows' bash is not enough: gcc then fails with "Cannot create
temporary file in C:\Windows\".)

```bash
WINDOWS/installer/build-deps.sh   # once: SoapyPlutoSDR, librtlsdr + SoapyRTLSDR -> /mingw64, Hamlib 4.5.5 rigctld -> build-win/deps
WINDOWS/installer/build-exes.sh   # make clean + make -> build-win/exe/pihpsdr-if.exe
WINDOWS/installer/stage.sh        # self-contained runtime folder build-win/stage
WINDOWS/installer/build-msi.sh    # -> build-win/piHPSDR-IF-<x.y.n>-x64.msi
```

For a quick development build a plain `make` is enough; `make WIN_CONSOLE=ON`
keeps a console window. Started from the source tree (where
`release/pihpsdr` exists), piHPSDR uses the source directory as working
directory, as on Linux.

The MSI version is `<tag>.<commits since tag>` from `git describe --match
'v[0-9]*.[0-9]*'` (e.g. `v3.1-19-g...` -> `3.1.19`), so every new commit
gives a higher version and the MSI upgrades an older install in place.

### Pitfalls (inherited from the deskHPSDR port)

* `strip --remove-section=.comment` on the exe (done in `build-exes.sh`):
  otherwise the PE section order is invalid and Windows refuses to start the
  exe on other PCs (error 193). `stage.sh` checks the section order of every
  staged exe/dll.
* Never build the Linux version (WSL!) in this same checkout: Linux ELF
  `.o`/`.a` files (`wdsp/libwdsp.a`, `libspecbleach/libspecbleach.a`) would
  be linked silently into the Windows exe. `build-exes.sh` always starts with
  `make clean`. Use a separate checkout for Linux builds.
* Text is drawn through fontconfig/FreeType (`WINDOWS/compat/cairo.h`,
  `win_fonts.c`, `PANGOCAIRO_BACKEND=fc`): MSYS2's cairo aborts in its
  DirectWrite toy-font path. piHPSDR's preferred fonts (Open Sans, ...) are
  not shipped; fontconfig falls back to the Windows fonts.

### RTL-SDR

librtlsdr (osmocom) and SoapyRTLSDR are not packaged by MSYS2; `build-deps.sh`
builds both into `/mingw64`. librtlsdr's bundled `getopt.c` is K&R C and does
not compile as C23 (GCC 15+ default), hence `-DCMAKE_C_FLAGS=-std=gnu17`. At
the 73.05 MHz IF an RTL-SDR (R820T) receives better than the Pluto. On the
user's PC the dongle needs the WinUSB driver (Zadig, see README-Windows.txt).

## 4. Installing

Run `build-win\piHPSDR-IF-<version>-x64.msi` (needs administrator rights,
installs per machine to `C:\Program Files\piHPSDR-IF`). Start Menu folder
"piHPSDR-IF" with: **piHPSDR-IF**, **Start rigctld (TS-570D)**, the read-me
and the settings folder; optional desktop shortcut **piHPSDR-IF**.
Settings and logs (`pihpsdr.stdout`, `pihpsdr.stderr`, props files, WDSP
wisdom) live in `%LOCALAPPDATA%\piHPSDR-IF`, separate from any piHPSDR or
deskHPSDR install. The first start computes the WDSP FFT wisdom (a few
minutes, progress shown in the start window).

Bundled: GTK3 runtime, WDSP, SoapySDR + PlutoSDR module
(`lib\SoapySDR\modules0.8-3\libPlutoSDRSupport.dll`, `librtlsdrSupport.dll`), libiio, librtlsdr + `rtl_test.exe`,
libad9361, `rigctld.exe` (Hamlib 4.5.5, static), `SoapySDRUtil.exe`,
`iio_info.exe`.

A USB-connected Pluto needs the Analog Devices PlutoSDR drivers (RNDIS +
USB, "PlutoSDR-M2k-USB-Drivers"). piHPSDR-IF opens a network Pluto as
`uri=ip:<host>`, default 192.168.2.1. Check the module with
`SoapySDRUtil --info` (lists `plutosdr`) in the `bin` folder.

## 5. rigctld for the TS-570D

The TS-570D must use 57600 baud (menu 35 = "576-1", then power-cycle the
radio). Find the COM port of the CAT cable in Device Manager, then either
use the Start Menu entry **Start rigctld (TS-570D)** (asks for the port), or
in a command prompt:

```bat
cd "C:\Program Files\piHPSDR-IF\bin"
rigctld -m 2004 -r COM3 -s 57600
```

(2004 = Hamlib model TS-570D; for COM10 and above use `\\.\COM10`.) Keep it
running, then start piHPSDR-IF; it connects to rigctld at 127.0.0.1:4532
(props `ifpan.rigctld_host` / `ifpan.rigctld_port`).

## 6. Audio

PortAudio (`src/portaudio.c`, as on macOS). Windows offers each sound
device once per host API, so the names are prefixed: "MME: ...",
"Windows DirectSound: ...", "Windows WASAPI: ...", "Windows WDM-KS: ...".
MME/DirectSound accept any device rate; WASAPI devices only show up if the
device is set to 48 kHz in the Windows sound settings.
