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
