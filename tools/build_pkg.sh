#!/usr/bin/env bash
# Build a real-NPDRM .pkg for a PSL1GHT homebrew.
#
# Stage 1 (Docker, ubuntu:22.04): make_self_npdrm — segfaults on the host's
# glibc 2.39, runs cleanly on glibc 2.35.
# Stage 2 (host, native): sfo.py + pkg.py + package_finalize — Python 3.12
# tools that work fine on the host.
#
# Required env:
#   ELF        path to the stripped ELF (build/build/<proj>.elf)
#   OUT_PKG    output path for the finalized pkg (e.g. <proj>.pkg)
#   STAGE      working dir (wiped at start; created fresh)
#   TITLE      sfo TITLE
#   APPID      9-char NP0xxxxxx
#   CONTENTID  UP0001-<APPID>_00-0000000000000000
#   ICON0      ICON0.PNG (optional; copied into pkg root if present)
#   SFOXML     sfo.xml template
#
# Optional:
#   PKGFILES   directory whose contents are copied into pkg/ (ICON0.PNG, etc.)

set -euo pipefail

: "${ELF:?ELF must be set}"
: "${OUT_PKG:?OUT_PKG must be set}"
: "${STAGE:?STAGE must be set}"
: "${TITLE:?TITLE must be set}"
: "${APPID:?APPID must be set}"
: "${CONTENTID:?CONTENTID must be set}"
: "${SFOXML:?SFOXML must be set}"

PS3DEV_BIN="${PS3DEV:-/opt/ps3dev}/bin"

for f in Struct.py sfo.py pkg.py fself.py make_self_npdrm package_finalize \
         pkgcrypt.cpython-312-x86_64-linux-gnu.so; do
    [ -f "$PS3DEV_BIN/$f" ] || { echo "missing toolchain bin: $PS3DEV_BIN/$f" >&2; exit 1; }
done
[ -f "$ELF" ]    || { echo "ELF not found: $ELF" >&2; exit 1; }
[ -f "$SFOXML" ] || { echo "sfo.xml not found: $SFOXML" >&2; exit 1; }
command -v docker >/dev/null || { echo "docker not on PATH" >&2; exit 1; }

rm -rf "$STAGE"
mkdir -p "$STAGE/pkg/USRDIR"

# Stage toolchain bins fresh each build so a host reboot can't break us.
cp "$PS3DEV_BIN"/{Struct.py,sfo.py,pkg.py,fself.py,make_self_npdrm,package_finalize,pkgcrypt.cpython-312-x86_64-linux-gnu.so} "$STAGE/"
cp "$ELF" "$STAGE/input.elf"

# Stage 1: sign EBOOT.BIN inside Ubuntu 22.04 (glibc 2.35 baseline).
docker run --rm \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    -e CONTENTID="$CONTENTID" \
    -v "$STAGE":/d -w /d ubuntu:22.04 bash -c '
  set -eu
  apt-get update -qq >/dev/null
  apt-get install -y -qq libgmp10 libssl3 zlib1g >/dev/null
  ./make_self_npdrm input.elf pkg/USRDIR/EBOOT.BIN "$CONTENTID"
  chown -R "$HOST_UID:$HOST_GID" /d
'

# Verify the EBOOT shape — bytes 8-11 must be 00 01 00 01.
hdr=$(xxd -p -l 12 "$STAGE/pkg/USRDIR/EBOOT.BIN" | tail -c 9 | head -c 8)
if [ "$hdr" != "00010001" ]; then
    echo "EBOOT.BIN signing failed — header bytes 8-11 = $hdr (want 00010001)" >&2
    exit 1
fi

# Stage 2: pkgfiles -> sfo -> pkg -> package_finalize (host-native).
if [ -n "${PKGFILES:-}" ] && [ -d "$PKGFILES" ]; then
    cp -rf "$PKGFILES"/. "$STAGE/pkg/"
fi
[ -n "${ICON0:-}" ] && [ -f "$ICON0" ] && cp -f "$ICON0" "$STAGE/pkg/ICON0.PNG"

cd "$STAGE"
python3 sfo.py --title "$TITLE" --appid "$APPID" -f "$SFOXML" pkg/PARAM.SFO
python3 pkg.py --contentid "$CONTENTID" pkg/ staged.pkg >/dev/null
cp staged.pkg staged.gnpdrm.pkg
./package_finalize staged.gnpdrm.pkg >/dev/null

mkdir -p "$(dirname "$OUT_PKG")"
cp staged.gnpdrm.pkg "$OUT_PKG"

echo "pkg ready: $OUT_PKG"
