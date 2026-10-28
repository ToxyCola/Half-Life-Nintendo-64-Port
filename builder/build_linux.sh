#!/bin/bash
# Builds hl64builder (the Linux ROM builder, x86_64) on Linux.
#
# Needs: the N64 toolchain + libdragon (preview, patched as in README) + tiny3d, a host C compiler with
# static zlib (Debian/Ubuntu: build-essential zlib1g-dev; Fedora: glibc-static zlib-static), Python 3.
#
#   T3D_INST=/path/to/tiny3d LIBDRAGON_SRC=/path/to/libdragon builder/build_linux.sh
#   -> builder/out/hl64builder
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=$(pwd)
: "${N64_INST:=/opt/libdragon}"
: "${T3D_INST:?set T3D_INST to your tiny3d checkout}"
: "${LIBDRAGON_SRC:?set LIBDRAGON_SRC to the libdragon source tree (for the Linux tools)}"
OUT=$REPO/builder/out
WORK=$OUT/work-linux
STAGE=$WORK/stage
VERSION=${HL64_VERSION:-$(git describe --tags --always 2>/dev/null || echo dev)}
export PATH=$N64_INST/bin:$PATH
rm -rf "$STAGE"; mkdir -p "$STAGE"/{tools,engine,hl64/tools,hl64/src,hl64/assets,licenses}

echo "== engine (N64 code from this checkout)"
rm -rf build
if [ ! -d filesystem ]; then mkdir -p filesystem; touch filesystem/.empty; fi
make -j"$(nproc)" T3D_INST="$T3D_INST" hl64.z64 >/dev/null
cp build/hl64.elf.stripped build/hl64.elf.sym "$STAGE/engine/"
cp "$N64_INST/mips64-elf/include/libdragon.version" "$STAGE/engine/"

echo "== libdragon tools for Linux (static)"
rm -rf "$WORK/libdragon"; cp -r "$LIBDRAGON_SRC" "$WORK/libdragon"
find "$WORK/libdragon/tools" \( -name '*.o' -o -name '*.d' -o -name '*.a' \) -delete
make -C "$WORK/libdragon/tools" -j"$(nproc)" N64_INST="$N64_INST" LDFLAGS="-static -pthread" \
     mksprite mkasset audioconv64 mkdfs n64tool ed64romconfig >/dev/null
for t in mksprite/mksprite mkasset/mkasset audioconv64/audioconv64 mkdfs/mkdfs n64tool ed64romconfig; do
    cp "$WORK/libdragon/tools/$t" "$STAGE/tools/"
done
cc -O2 -static -o "$STAGE/tools/navgen" tools/navgen.c -lm
cp "$LIBDRAGON_SRC/LICENSE.md" "$STAGE/licenses/libdragon-LICENSE.md" 2>/dev/null || cp "$LIBDRAGON_SRC/LICENSE" "$STAGE/licenses/libdragon-LICENSE" 2>/dev/null || true

echo "== converters"
cp tools/*.py "$STAGE/hl64/tools/"
cp src/npc.c "$STAGE/hl64/src/"
cp assets/*.png "$STAGE/hl64/assets/"
cp valve_sdk/LICENSE "$STAGE/licenses/valve-sdk-LICENSE" 2>/dev/null || true
cp README.md "$STAGE/licenses/HL64-README.md"

echo "== hl64builder ($VERSION)"
cc -O2 -static -o "$WORK/stub" -DHL64_VERSION="\"$VERSION\"" builder/hl64builder_linux.c -lz
python3 builder/pack.py "$WORK/stub" "$STAGE" "$OUT/hl64builder"
chmod +x "$OUT/hl64builder"
