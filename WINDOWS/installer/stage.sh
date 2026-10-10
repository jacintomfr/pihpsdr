#!/bin/bash
#
# Collect pihpsdr-if.exe and EVERYTHING it needs at run time into a
# self-contained folder (default: build-win/stage) that works on a Windows
# machine without MSYS2. Run from the MSYS2 MINGW64 shell, in the piHPSDR
# source directory, after build-exes.sh (or a plain "make").
#
# Layout follows the usual relocatable GTK convention, because GLib/GTK,
# gdk-pixbuf, fontconfig and SoapySDR all locate their data relative to the
# directory *above* the bin/ folder that holds their DLL:
#
#   bin/        pihpsdr-if.exe, all DLLs, rigctld.exe (Hamlib, CAT for the
#               TS-570D), start-rigctld-TS570D.bat, SoapySDRUtil.exe and
#               iio_info.exe (diagnostics)
#   lib/        gdk-pixbuf loaders, SoapySDR modules (PlutoSDR support)
#   share/      GLib schemas, icon themes
#   etc/fonts/  fontconfig configuration (text is drawn via fontconfig)
#
set -e
MINGW=/mingw64
STAGE=${1:-build-win/stage}
DEPS=${DEPS:-build-win/deps}
INST=WINDOWS/installer
EXE=${EXE:-build-win/exe/pihpsdr-if.exe}

if [ ! -f "$EXE" ]; then
  echo "WARNING: $EXE not found (build-exes.sh) - staging ./pihpsdr-if.exe from a plain make"
  EXE=pihpsdr-if.exe
fi
[ -f "$EXE" ] || { echo "$EXE not found - run WINDOWS/installer/build-exes.sh first"; exit 1; }

RIGCTLD=$DEPS/hamlib/tests/.libs/rigctld.exe
[ -f "$RIGCTLD" ] || RIGCTLD=$DEPS/hamlib/tests/rigctld.exe
[ -f "$RIGCTLD" ] || { echo "rigctld.exe not found - run WINDOWS/installer/build-deps.sh first"; exit 1; }

SOAPYMOD=lib/SoapySDR/modules0.8-3
[ -f "$MINGW/$SOAPYMOD/libPlutoSDRSupport.dll" ] || \
  { echo "SoapyPlutoSDR module not found - run WINDOWS/installer/build-deps.sh first"; exit 1; }

rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/share/glib-2.0/schemas" "$STAGE/share/icons" "$STAGE/etc"

cp "$EXE" "$STAGE/bin/pihpsdr-if.exe"
cp "$RIGCTLD" "$STAGE/bin/rigctld.exe"
cp "$INST/start-rigctld-TS570D.bat" "$STAGE/bin/"

# --- run-time loaded modules (not visible to ldd of the exe) ---------------
PIXBUF=lib/gdk-pixbuf-2.0/2.10.0
mkdir -p "$STAGE/$PIXBUF"
cp -r "$MINGW/$PIXBUF/loaders" "$STAGE/$PIXBUF/"
cp "$MINGW/$PIXBUF/loaders.cache" "$STAGE/$PIXBUF/"     # uses relative paths

# SoapySDR finds its modules relative to libSoapySDR.dll (bin\..\lib\...).
# Shipped: the PlutoSDR module, and the RTL-SDR module when built (an
# RTL-SDR dongle can also sit on the IF; it needs the WinUSB driver).
mkdir -p "$STAGE/$SOAPYMOD"
cp "$MINGW/$SOAPYMOD/libPlutoSDRSupport.dll" "$STAGE/$SOAPYMOD/"
[ -f "$MINGW/$SOAPYMOD/librtlsdrSupport.dll" ] && cp "$MINGW/$SOAPYMOD/librtlsdrSupport.dll" "$STAGE/$SOAPYMOD/"

# helper programs GLib may spawn, plus SoapySDR / libiio / RTL-SDR diagnostics tools
for x in gspawn-win64-helper.exe gspawn-win64-helper-console.exe SoapySDRUtil.exe iio_info.exe rtl_test.exe; do
  [ -f "$MINGW/bin/$x" ] && cp "$MINGW/bin/$x" "$STAGE/bin/"
done

# --- recursive DLL closure of everything staged so far ---------------------
# ldd resolves the complete dependency tree; keep only DLLs from /mingw64
# (system DLLs from C:\Windows are present on every machine).
for pass in 1 2 3; do
  find "$STAGE" -iname '*.dll' -o -iname '*.exe' | while read -r f; do
    ldd "$f" 2>/dev/null | awk '{print $3}' | grep -i "^$MINGW/bin/" || true
  done | sort -u | while read -r dll; do
    [ -f "$STAGE/bin/$(basename "$dll")" ] || cp "$dll" "$STAGE/bin/"
  done
done

# --- data files -------------------------------------------------------------
cp "$MINGW"/share/glib-2.0/schemas/org.gtk.*.xml "$STAGE/share/glib-2.0/schemas/"
glib-compile-schemas "$STAGE/share/glib-2.0/schemas"
cp -r "$MINGW/share/icons/Adwaita" "$MINGW/share/icons/hicolor" "$STAGE/share/icons/"
cp -r "$MINGW/etc/fonts" "$STAGE/etc/"
mkdir -p "$STAGE/etc/gtk-3.0"

# CA bundle for libcurl (https in the server menu): the MSYS2 default path
# does not exist on a user machine; curl on Windows looks for
# curl-ca-bundle.crt next to the exe.
cp "$MINGW/etc/ssl/certs/ca-bundle.crt" "$STAGE/bin/curl-ca-bundle.crt"

cp COPYING "$STAGE/COPYING.txt" 2>/dev/null || true
cp WINDOWS/README-Windows.txt "$STAGE/"

# --- sanity check: PE section order (error 193 pitfall, WINDOWS/BUILD.md) --
bad=0
for f in "$STAGE"/bin/*.exe "$STAGE"/bin/*.dll "$STAGE"/$SOAPYMOD/*.dll; do
  objdump -h "$f" | awk -v f="$f" '
    /^ *[0-9]+ / { n=$1+0; vma=strtonum("0x" $4);
                   if (n>0 && vma < prev) { print f, "BAD SECTION ORDER"; bad=1 }
                   prev=vma }
    END { exit bad }' || bad=1
done
[ "$bad" = 0 ] || { echo "ERROR: an image with bad section order was staged"; exit 1; }

echo "Staged into $STAGE: $(find "$STAGE" -type f | wc -l) files, $(du -sh "$STAGE" | cut -f1)"
