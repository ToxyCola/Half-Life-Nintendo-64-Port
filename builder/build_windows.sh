#!/bin/bash
# Builds HL64Builder.exe (the Windows ROM builder) on Linux.
#
# Needs: the N64 toolchain + libdragon (preview, patched as in README) + tiny3d, mingw-w64
# (x86_64-w64-mingw32-gcc) with zlib (Debian/Ubuntu: mingw-w64 libz-mingw-w64-dev), Python 3, pip, curl.
#
#   T3D_INST=/path/to/tiny3d LIBDRAGON_SRC=/path/to/libdragon builder/build_windows.sh
#   -> builder/out/HL64Builder.exe
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=$(pwd)
: "${N64_INST:=/opt/libdragon}"
: "${T3D_INST:?set T3D_INST to your tiny3d checkout}"
: "${LIBDRAGON_SRC:?set LIBDRAGON_SRC to the libdragon source tree (for the Windows tools)}"
# Python 3.12 + numpy 1.26: numpy 2.x needs ucrtbase's crealf at import, which Wine doesn't have,
# so this combination also runs under Wine (it's how the builder is tested)
PY_VER=3.12.10
OUT=$REPO/builder/out
WORK=$OUT/work
STAGE=$WORK/stage
VERSION=${HL64_VERSION:-$(git describe --tags --always 2>/dev/null || echo dev)}
export PATH=$N64_INST/bin:$PATH
rm -rf "$STAGE"; mkdir -p "$STAGE"/{tools,engine,hl64/tools,hl64/src,hl64/assets,licenses} "$WORK/dl"

echo "== engine (N64 code from this checkout)"
rm -rf build
if [ ! -d filesystem ]; then mkdir -p filesystem; touch filesystem/.empty; fi
make -j"$(nproc)" T3D_INST="$T3D_INST" hl64.z64 >/dev/null
cp build/hl64.elf.stripped build/hl64.elf.sym "$STAGE/engine/"
cp "$N64_INST/mips64-elf/include/libdragon.version" "$STAGE/engine/"

echo "== libdragon tools for Windows"
rm -rf "$WORK/libdragon"; cp -r "$LIBDRAGON_SRC" "$WORK/libdragon"
find "$WORK/libdragon/tools" \( -name '*.o' -o -name '*.d' -o -name '*.a' \) -delete
make -C "$WORK/libdragon/tools" -j"$(nproc)" N64_INST="$N64_INST" CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++ \
     AR=x86_64-w64-mingw32-ar LDFLAGS="-static -pthread" mksprite mkasset audioconv64 mkdfs n64tool ed64romconfig >/dev/null
for t in mksprite/mksprite mkasset/mkasset audioconv64/audioconv64 mkdfs/mkdfs n64tool ed64romconfig; do
    cp "$WORK/libdragon/tools/$t.exe" "$STAGE/tools/"
done
x86_64-w64-mingw32-gcc -O2 -static -o "$STAGE/tools/navgen.exe" tools/navgen.c -lm
cp "$LIBDRAGON_SRC/LICENSE.md" "$STAGE/licenses/libdragon-LICENSE.md" 2>/dev/null || cp "$LIBDRAGON_SRC/LICENSE" "$STAGE/licenses/libdragon-LICENSE" 2>/dev/null || true

echo "== Python $PY_VER (embeddable) + numpy + Pillow"
[ -f "$WORK/dl/python-$PY_VER.zip" ] || curl -sfo "$WORK/dl/python-$PY_VER.zip" "https://www.python.org/ftp/python/$PY_VER/python-$PY_VER-embed-amd64.zip"
mkdir -p "$STAGE/python/Lib/site-packages"
python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$WORK/dl/python-$PY_VER.zip" "$STAGE/python"
pip download -q --only-binary=:all: --platform win_amd64 --python-version 3.12 --implementation cp \
    "numpy==1.26.4" "pillow==12.3.0" -d "$WORK/dl/wheels"
for w in "$WORK"/dl/wheels/*.whl; do
    python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$w" "$STAGE/python/Lib/site-packages"
done
PTH=$(ls "$STAGE"/python/python3*._pth)
ZIPN=$(basename "$(ls "$STAGE"/python/python3*.zip)")
printf '%s\r\n.\r\nLib\\site-packages\r\n..\\hl64\\tools\r\nimport site\r\n' "$ZIPN" > "$PTH"
cp "$STAGE/python/LICENSE.txt" "$STAGE/licenses/python-LICENSE.txt" 2>/dev/null || true

echo "== converters"
cp tools/*.py "$STAGE/hl64/tools/"
cp src/npc.c "$STAGE/hl64/src/"
cp assets/*.png "$STAGE/hl64/assets/"
cp valve_sdk/LICENSE "$STAGE/licenses/valve-sdk-LICENSE" 2>/dev/null || true
cp README.md "$STAGE/licenses/HL64-README.md"

echo "== HL64Builder.exe ($VERSION)"
x86_64-w64-mingw32-gcc -O2 -static -o "$WORK/stub.exe" -DHL64_VERSION="\"$VERSION\"" builder/hl64builder.c -lz -ladvapi32 -lshell32
python3 builder/pack.py "$WORK/stub.exe" "$STAGE" "$OUT/HL64Builder.exe"
