#!/bin/bash
#
# Build piHPSDR-IF-<version>-x64.msi from the staged folder.
# Run from the MSYS2 MINGW64 shell in the piHPSDR source directory:
#
#   WINDOWS/installer/build-exes.sh       # -> build-win/exe/pihpsdr-if.exe
#   WINDOWS/installer/stage.sh            # collect exe + runtime into build-win/stage
#   WINDOWS/installer/build-msi.sh        # -> build-win/piHPSDR-IF-<ver>-x64.msi
#
# Needs WiX Toolset v3.x (candle/light/heat), default install location below.
#
set -e
WIX=${WIX_BIN:-"/c/Program Files (x86)/WiX Toolset v3.14/bin"}
STAGE=build-win/stage
OUT=build-win
INST=WINDOWS/installer

[ -f "$STAGE/bin/pihpsdr-if.exe" ] || { echo "run WINDOWS/installer/stage.sh first"; exit 1; }
[ -x "$WIX/candle.exe" ] || { echo "WiX v3 not found in $WIX (set WIX_BIN)"; exit 1; }

# MSI versions are numeric (x.y.z, each field 0-255/255/65535) and MUST keep
# increasing across builds, or MajorUpgrade refuses to install (lesson from
# the deskHPSDR Windows build). Anchor to the nearest piHPSDR release tag
# (vX.Y, e.g. v3.1) and use the commit count since that tag as 3rd field:
# v3.1-19-gHASH -> 3.1.19. MSI ignores a 4th field when comparing versions,
# so the count must be in the 3rd one.
DESC=$(git describe --tags --long --match 'v[0-9]*.[0-9]*' 2>/dev/null || true)
if [[ "$DESC" =~ ^v([0-9]+)\.([0-9]+)(\.[0-9]+)?-([0-9]+)-g ]]; then
  VERSION="${BASH_REMATCH[1]}.${BASH_REMATCH[2]}.${BASH_REMATCH[4]}"
else
  VERSION=0.0.0
fi
echo "MSI version $VERSION (git describe: $DESC)"

W() { cygpath -w "$1"; }
# harvest the whole stage folder; -ag: Guid="*" (stable, derived from path)
"$WIX/heat.exe" dir "$(W $STAGE)" -nologo -cg StageFiles -dr APPLICATIONFOLDER \
    -ag -srd -sreg -scom -sfrag -var var.StageDir -out "$(W $OUT/stagefiles.wxs)"
"$WIX/candle.exe" -nologo -arch x64 \
    -dVersion="$VERSION" -dStageDir="$(W $STAGE)" -dInstallerDir="$(W $INST)" \
    -out "$(W $OUT)\\" "$(W $INST/pihpsdr-if.wxs)" "$(W $OUT/stagefiles.wxs)"
"$WIX/light.exe" -nologo -ext WixUIExtension -sice:ICE61 -sice:ICE60 -sice:ICE91 \
    -out "$(W $OUT/piHPSDR-IF-$VERSION-x64.msi)" "$(W $OUT/pihpsdr-if.wixobj)" "$(W $OUT/stagefiles.wixobj)"
ls -la "$OUT/piHPSDR-IF-$VERSION-x64.msi"
