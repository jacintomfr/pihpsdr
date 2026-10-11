#!/usr/bin/env bash
#
# Build the RADE V1 library (rade_c) + the Opus FARGAN/LPCNet it needs as
# static libraries, for piHPSDR-IF: Windows (MSYS2 MinGW64) or Linux (incl. Raspberry Pi).
#
# Result (the Makefile switches RADE on when it exists):
#   rade-libs/lib/librade_static.a   RADE V1+V2, neural weights compiled in
#   rade-libs/lib/libopus.a          Opus with FARGAN/DRED/OSCE (patched nnet)
#   rade-libs/include/               all rade_c/src/*.h (rade_api.h pulls in
#                                    rade_tx.h/rade_rx.h/rade_dsp.h/rade_core.h)
#   rade-libs/opus-include/          Opus config.h + celt/ dnn/ include/ headers
#                                    (needed by rade_core.h and src/rade_shim.c)
#
# Needs network (rade_c clone, Opus zip, Opus DNN model tarball) and the
# packages cmake make patch git curl autoconf automake libtool gettext(-devel)
# (MSYS2: run it in the MINGW64 shell; Linux: also libspeexdsp-dev for the
# program itself). Takes ~6 minutes on one core (four ~25-40 MB weight .c
# files + Opus). Use "./build-rade.sh --force" to rebuild. If the rade_c repo
# is not accessible, set RADE_C_DIR=/path/to/rade_c (same commit) instead.
# Taken from the deskHPSDR Windows RADE port (docs/RADE-INTEGRATION.md there).
#
# NOTE: the libraries are built generic (SSE2 baseline, no -mavx2); rade_c
# warns "Only SSE and SSE2 are available". For an AVX2 variant add
# -mavx2 -mfma to CMAKE_C_FLAGS below and to the Opus CFLAGS (not tried yet).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RADE_URL="https://github.com/jacintomfr/rade_c"
RADE_BRANCH="hpsdr-rs-local"
RADE_COMMIT="c394514738bc407930f80ccdcd28fc8531c28a1b"

SRC_DIR="${SCRIPT_DIR}/rade-src/rade_c"
BUILD_DIR="${SCRIPT_DIR}/rade-build"
TARGET_DIR="${SCRIPT_DIR}/rade-libs"

if [[ "${1:-}" != "--force" && -f "${TARGET_DIR}/lib/librade_static.a" \
      && -f "${TARGET_DIR}/lib/libopus.a" ]]; then
    echo "rade-libs already built (use --force to rebuild)."
    exit 0
fi

# REQUIRED: an inherited ACLOCAL_PATH is mangled by native cmake and breaks
# autoreconf in Opus' autogen.sh ("progtest.m4 does not exist").
unset ACLOCAL_PATH

if [[ -n "${RADE_C_DIR:-}" ]]; then
    # Use an existing rade_c tree (e.g. vendor/rade_c of hpsdr-rs-git, needed
    # while the GitHub repository is private).
    SRC_DIR="${RADE_C_DIR}"
else
    if [[ ! -d "${SRC_DIR}/.git" ]]; then
        echo "Cloning rade_c ${RADE_COMMIT}..."
        mkdir -p "$(dirname "${SRC_DIR}")"
        GIT_TERMINAL_PROMPT=0 git clone --branch "${RADE_BRANCH}" "${RADE_URL}" "${SRC_DIR}"
    fi
    git -C "${SRC_DIR}" checkout --quiet "${RADE_COMMIT}"
fi

rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"
case "$(uname -s)" in
    MINGW*|MSYS*) GENERATOR="MSYS Makefiles" ;;
    *)            GENERATOR="Unix Makefiles" ;;
esac
cmake -G "${GENERATOR}" -DCMAKE_BUILD_TYPE=Release \
      -DRADE_C_DIR="${SRC_DIR}" "${SCRIPT_DIR}/rade"
cmake --build . --target rade_static -- -j"$(nproc 2>/dev/null || echo 2)"

OPUS_DIR="${BUILD_DIR}/build_opus-prefix/src/build_opus"
if [[ ! -f "${OPUS_DIR}/.libs/libopus.a" ]]; then
    echo "libopus.a not found in ${OPUS_DIR}/.libs" >&2
    exit 1
fi

rm -rf "${TARGET_DIR}"
mkdir -p "${TARGET_DIR}/lib" "${TARGET_DIR}/include" "${TARGET_DIR}/opus-include"
cp "${BUILD_DIR}/librade_static.a" "${TARGET_DIR}/lib/"
cp "${OPUS_DIR}/.libs/libopus.a" "${TARGET_DIR}/lib/"
cp "${SRC_DIR}"/src/*.h "${TARGET_DIR}/include/"

cp "${OPUS_DIR}/config.h" "${TARGET_DIR}/opus-include/"
# all headers, keeping sub-directories (celt/x86/x86cpu.h, dnn/x86/...)
(cd "${OPUS_DIR}" && find dnn celt include -name "*.h" -exec cp --parents {} "${TARGET_DIR}/opus-include" \;)

echo "RADE libraries installed in ${TARGET_DIR}"
