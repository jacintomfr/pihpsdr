#!/bin/bash
#
# Build the two run-time helpers that MSYS2 does not package, once per
# build machine. Run from the MSYS2 MINGW64 shell in the piHPSDR source
# directory (see WINDOWS/BUILD.md):
#
#   WINDOWS/installer/build-deps.sh
#
#  1. SoapyPlutoSDR (the "plutosdr" SoapySDR module, ADALM-Pluto support),
#     built against MSYS2's SoapySDR + libiio + libad9361-iio and installed
#     into /mingw64/lib/SoapySDR/modules0.8-3, i.e. exactly where stage.sh
#     collects the Soapy modules from (same as LimeSuite's libLMS7Support.dll
#     in the deskHPSDR Windows build).
#
#  2. Hamlib rigctld.exe (CAT for the IF panadapter: piHPSDR-IF talks to
#     rigctld over TCP, rigctld owns the COM port of the TS-570D). Hamlib
#     is built statically (same configure options as deskHPSDR's
#     build-rigctld.sh), so rigctld.exe needs no libhamlib DLL; stage.sh
#     copies it next to pihpsdr-if.exe.
#
# Sources are cloned into build-win/deps (git-ignored). Re-running the
# script skips whatever is already built; delete build-win/deps/<name> to
# force a rebuild.
#
set -e
MINGW=/mingw64
DEPS=${DEPS:-build-win/deps}
JOBS=${JOBS:-$(nproc)}
HAMLIB_TAG=${HAMLIB_TAG:-4.5.5}        # release tag; same version as Ubuntu 24.04 (hamlib-utils 4.5.5)
SOAPYPLUTO_GIT=https://github.com/pothosware/SoapyPlutoSDR.git
HAMLIB_GIT=https://github.com/Hamlib/Hamlib.git

mkdir -p "$DEPS"
DEPS=$(cd "$DEPS" && pwd)

pkg-config --exists soapysdr || { echo "SoapySDR not found (pkg-config soapysdr) - see WINDOWS/BUILD.md section 2"; exit 1; }
for p in mingw-w64-x86_64-libiio mingw-w64-x86_64-libad9361-iio mingw-w64-x86_64-cmake; do
  pacman -Q "$p" >/dev/null 2>&1 || { echo "missing package $p - see WINDOWS/BUILD.md section 2"; exit 1; }
done

# ---------------------------------------------------------------- SoapyPlutoSDR
SOAPYMOD=$(ls -d "$MINGW"/lib/SoapySDR/modules* | head -1)
if [ -f "$SOAPYMOD/libPlutoSDRSupport.dll" ]; then
  echo "==> SoapyPlutoSDR already installed: $SOAPYMOD/libPlutoSDRSupport.dll"
else
  echo "==> SoapyPlutoSDR"
  [ -d "$DEPS/SoapyPlutoSDR" ] || git clone "$SOAPYPLUTO_GIT" "$DEPS/SoapyPlutoSDR"
  cmake -S "$DEPS/SoapyPlutoSDR" -B "$DEPS/SoapyPlutoSDR/build" -G "MSYS Makefiles" \
        -DCMAKE_INSTALL_PREFIX=$MINGW -DCMAKE_BUILD_TYPE=Release
  cmake --build "$DEPS/SoapyPlutoSDR/build" -j"$JOBS"
  cmake --install "$DEPS/SoapyPlutoSDR/build"
  ls -la "$SOAPYMOD"/libPlutoSDRSupport.dll
fi

# ---------------------------------------------------------------- RTL-SDR
#
# librtlsdr (osmocom) + SoapyRTLSDR (the "rtlsdr" SoapySDR module), so an
# RTL-SDR dongle can be used on the IF instead of the Pluto (its tuner
# covers 73.05 MHz). Not packaged by MSYS2. The dongle needs the WinUSB
# driver (Zadig) on the user's PC.
#
RTLSDR_GIT=https://github.com/osmocom/rtl-sdr.git
SOAPYRTL_GIT=https://github.com/pothosware/SoapyRTLSDR.git
if [ -f "$MINGW/bin/librtlsdr.dll" ]; then
  echo "==> librtlsdr already installed: $MINGW/bin/librtlsdr.dll"
else
  echo "==> librtlsdr (osmocom rtl-sdr)"
  pacman -Q mingw-w64-x86_64-libusb >/dev/null 2>&1 || { echo "missing package mingw-w64-x86_64-libusb"; exit 1; }
  [ -d "$DEPS/rtl-sdr" ] || git clone "$RTLSDR_GIT" "$DEPS/rtl-sdr"
  # -std=gnu17: the bundled K&R-style getopt.c does not compile as C23,
  # GCC 15+'s default ("too many arguments to function 'getenv'")
  cmake -S "$DEPS/rtl-sdr" -B "$DEPS/rtl-sdr/build" -G "MSYS Makefiles" \
        -DCMAKE_INSTALL_PREFIX=$MINGW -DCMAKE_BUILD_TYPE=Release -DINSTALL_UDEV_RULES=OFF \
        -DCMAKE_C_FLAGS="-std=gnu17"
  cmake --build "$DEPS/rtl-sdr/build" -j"$JOBS"
  cmake --install "$DEPS/rtl-sdr/build"
fi
if [ -f "$SOAPYMOD/librtlsdrSupport.dll" ]; then
  echo "==> SoapyRTLSDR already installed: $SOAPYMOD/librtlsdrSupport.dll"
else
  echo "==> SoapyRTLSDR"
  [ -d "$DEPS/SoapyRTLSDR" ] || git clone "$SOAPYRTL_GIT" "$DEPS/SoapyRTLSDR"
  cmake -S "$DEPS/SoapyRTLSDR" -B "$DEPS/SoapyRTLSDR/build" -G "MSYS Makefiles" \
        -DCMAKE_INSTALL_PREFIX=$MINGW -DCMAKE_BUILD_TYPE=Release
  cmake --build "$DEPS/SoapyRTLSDR/build" -j"$JOBS"
  cmake --install "$DEPS/SoapyRTLSDR/build"
  ls -la "$SOAPYMOD"/librtlsdrSupport.dll
fi

#
# libmirisdr-5 + SoapyMiri (the "soapyMiri" SoapySDR module): Mirics
# MSi2500 / MSi001 receivers, i.e. the SDRplay RSP1 and its clones (USB
# 1DF7:2500), without the closed SDRplay API. GPL, so they can be shipped.
# The device needs the **libusbK** driver (Zadig) on the user's PC and
# isochronous transfers (piHPSDR asks for them): checked 2026-10-11 with an
# MSi2500 clone, WinUSB gave no samples in bulk mode and crashed libusb in
# isochronous mode, libusbK + isochronous streams 1.536 MS/s and reopens
# without replugging. libmirisdr's two command-line tools use POSIX signals
# and do not build with MinGW; only the library is built and installed.
#
MIRISDR_GIT=https://github.com/ericek111/libmirisdr-5.git
SOAPYMIRI_GIT=https://github.com/ericek111/SoapyMiri.git
if [ -f "$MINGW/bin/libmirisdr.dll" ]; then
  echo "==> libmirisdr already installed: $MINGW/bin/libmirisdr.dll"
else
  echo "==> libmirisdr-5"
  [ -d "$DEPS/libmirisdr-5" ] || git clone "$MIRISDR_GIT" "$DEPS/libmirisdr-5"
  sed -i 's/^add_executable(miri_sdr miri_sdr.c)/add_executable(miri_sdr EXCLUDE_FROM_ALL miri_sdr.c)/;
          s/^add_executable(miri_fm miri_fm.c)/add_executable(miri_fm EXCLUDE_FROM_ALL miri_fm.c)/;
          s/^set(INSTALL_TARGETS mirisdr_shared mirisdr_static miri_sdr miri_fm)/set(INSTALL_TARGETS mirisdr_shared mirisdr_static)/' \
      "$DEPS/libmirisdr-5/src/CMakeLists.txt"
  cmake -S "$DEPS/libmirisdr-5" -B "$DEPS/libmirisdr-5/build" -G "MSYS Makefiles" \
        -DCMAKE_INSTALL_PREFIX=$MINGW -DCMAKE_BUILD_TYPE=Release
  cmake --build "$DEPS/libmirisdr-5/build" -j"$JOBS"
  cmake --install "$DEPS/libmirisdr-5/build"
fi
if [ -f "$SOAPYMOD/libsoapyMiriSupport.dll" ]; then
  echo "==> SoapyMiri already installed: $SOAPYMOD/libsoapyMiriSupport.dll"
else
  echo "==> SoapyMiri"
  [ -d "$DEPS/SoapyMiri" ] || git clone "$SOAPYMIRI_GIT" "$DEPS/SoapyMiri"
  # keep the transfer mode as a copy (it pointed into setupStream's
  # arguments, already gone when activateStream used it again)
  git -C "$DEPS/SoapyMiri" apply --check "$(pwd)/WINDOWS/installer/soapymiri-transfer-mode.patch" 2>/dev/null \
    && git -C "$DEPS/SoapyMiri" apply "$(pwd)/WINDOWS/installer/soapymiri-transfer-mode.patch"
  cmake -S "$DEPS/SoapyMiri" -B "$DEPS/SoapyMiri/build" -G "MSYS Makefiles" \
        -DCMAKE_INSTALL_PREFIX=$MINGW -DCMAKE_BUILD_TYPE=Release
  cmake --build "$DEPS/SoapyMiri/build" -j"$JOBS"
  cmake --install "$DEPS/SoapyMiri/build"
  ls -la "$SOAPYMOD"/libsoapyMiriSupport.dll
fi

# ---------------------------------------------------------------- Hamlib rigctld
HAMLIB=$DEPS/hamlib
if [ -f "$HAMLIB/tests/.libs/rigctld.exe" ] || [ -f "$HAMLIB/tests/rigctld.exe" ]; then
  echo "==> rigctld already built in $HAMLIB/tests"
else
  echo "==> Hamlib $HAMLIB_TAG (rigctld)"
  [ -d "$HAMLIB" ] || git clone "$HAMLIB_GIT" "$HAMLIB"
  cd "$HAMLIB"
  git checkout -q "$HAMLIB_TAG"
  if [ ! -f Makefile ]; then
    [ -f configure ] || ./bootstrap
    ./configure --enable-static --disable-shared --disable-winradio \
                --without-readline --without-libusb --without-indi \
                --without-cxx-binding --without-xml-support
  fi
  make -j"$JOBS"
  cd - >/dev/null
fi
RIGCTLD=$HAMLIB/tests/rigctld.exe
[ -f "$HAMLIB/tests/.libs/rigctld.exe" ] && RIGCTLD=$HAMLIB/tests/.libs/rigctld.exe
"$RIGCTLD" --version
echo "Done: rigctld = $RIGCTLD"
