#!/bin/bash
#
# Build pihpsdr-if.exe for the Windows installer.
# Run from the MSYS2 MINGW64 shell in the piHPSDR source directory:
#
#   WINDOWS/installer/build-deps.sh    # once: SoapyPlutoSDR + Hamlib rigctld
#   WINDOWS/installer/build-exes.sh    # -> build-win/exe/pihpsdr-if.exe
#   WINDOWS/installer/stage.sh         # collect exe + runtime into build-win/stage
#   WINDOWS/installer/build-msi.sh     # -> build-win/piHPSDR-IF-<ver>-x64.msi
#
# Unlike the deskHPSDR Windows build there is only one executable (no
# AVX2/generic pair + CPU launcher): piHPSDR's Makefile never uses
# -march=native, so the default MinGW64 code (plain x86-64) runs on every
# 64-bit PC, exactly like the Linux build does.
#
# A full "make clean" is done first (WDSP and libspecbleach included), so no
# object file of an earlier (e.g. different-option) build can leak in.
# NEVER build the Linux version in this same checkout (WSL etc.): its ELF
# .o/.a files would end up in the Windows link (see WINDOWS/BUILD.md).
#
set -e
OUT=build-win/exe
JOBS=${JOBS:-$(nproc)}
EXE=pihpsdr-if.exe
mkdir -p "$OUT"
rm -f "$OUT"/*.exe

case "$(uname -s)" in
  MINGW64*) ;;
  *) echo "run this from the MSYS2 MINGW64 shell (see WINDOWS/BUILD.md)"; exit 1 ;;
esac

make clean >/dev/null
make -j"$JOBS" IFPAN=ON SOAPYSDR=ON
[ -f "$EXE" ] || { echo "make did not produce $EXE"; exit 1; }

# The MinGW64 linker places the .comment section (the compiler's "GCC: ..."
# identification string, no code) at a bogus virtual address on the big
# multi-object link; the Windows PE loader then refuses the image with
# error 193 (ERROR_BAD_EXE_FORMAT) on other PCs. Same fix as in the
# deskHPSDR Windows build: remove that metadata-only section.
strip --remove-section=.comment "$EXE"
cp "$EXE" "$OUT/"
ls -la "$OUT"
