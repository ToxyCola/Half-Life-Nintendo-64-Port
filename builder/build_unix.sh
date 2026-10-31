#!/bin/bash
# Builds the Half-Life 64 ROM builder for Linux or macOS (the same window as HL64Builder.exe,
# tools/builder_gui.py on a private Python with Tk, numpy and Pillow).
#
#   LIBDRAGON_SRC=/path/to/libdragon [ENGINE_DIR=dir | T3D_INST=/path/to/tiny3d] builder/build_unix.sh linux-x86_64
#   -> builder/out/HL64Builder-linux-x86_64.tar.gz   (HL64Builder/ with the HL64Builder launcher)
#   LIBDRAGON_SRC=... ENGINE_DIR=... builder/build_unix.sh macos-arm64     (run on a Mac; also macos-x86_64)
#   -> builder/out/HL64Builder-macos-arm64.zip       (HL64Builder.app)
#
# ENGINE_DIR holds an already built engine (hl64.elf.stripped, hl64.elf.sym, libdragon.version): a Mac
# has no N64 toolchain, so the release workflow builds the engine on Linux and hands it over.
# Without ENGINE_DIR the engine is built here (N64_INST toolchain + libdragon + tiny3d).
set -euo pipefail
TARGET=${1:?linux-x86_64, linux-arm64, macos-arm64 or macos-x86_64}
cd "$(dirname "$0")/.."
REPO=$(pwd)
: "${LIBDRAGON_SRC:?set LIBDRAGON_SRC to the libdragon source tree (for the host tools)}"
PY_REL=20251014
PY_VER=3.12.12
OUT=$REPO/builder/out
WORK=$OUT/work-$TARGET
VERSION=${HL64_VERSION:-$(git describe --tags --always 2>/dev/null || echo dev)}
case "$TARGET" in
    linux-x86_64) TRIPLE=x86_64-unknown-linux-gnu ;;
    linux-arm64)  TRIPLE=aarch64-unknown-linux-gnu ;;
    macos-arm64)  TRIPLE=aarch64-apple-darwin ;;
    macos-x86_64) TRIPLE=x86_64-apple-darwin ;;
    *) echo "unknown target $TARGET"; exit 1 ;;
esac
MAC=0; [[ $TARGET == macos-* ]] && MAC=1
if [ $MAC = 1 ]; then
    APP=$WORK/HL64Builder.app
    STAGE=$APP/Contents/Resources
else
    STAGE=$WORK/HL64Builder
fi
rm -rf "$WORK"; mkdir -p "$STAGE"/{tools,engine,hl64/tools,hl64/src,hl64/assets,licenses} "$WORK/dl"

echo "== engine"
if [ -n "${ENGINE_DIR:-}" ]; then
    cp "$ENGINE_DIR"/hl64.elf.stripped "$ENGINE_DIR"/hl64.elf.sym "$ENGINE_DIR"/libdragon.version "$STAGE/engine/"
else
    : "${N64_INST:=/opt/libdragon}" "${T3D_INST:?set T3D_INST (or ENGINE_DIR)}"
    export PATH=$N64_INST/bin:$PATH
    rm -rf build
    if [ ! -d filesystem ]; then mkdir -p filesystem; touch filesystem/.empty; fi
    make -j"$(getconf _NPROCESSORS_ONLN)" T3D_INST="$T3D_INST" hl64.z64 >/dev/null
    cp build/hl64.elf.stripped build/hl64.elf.sym "$STAGE/engine/"
    cp "$N64_INST/mips64-elf/include/libdragon.version" "$STAGE/engine/"
fi

echo "== libdragon tools ($TARGET)"
cp -r "$LIBDRAGON_SRC" "$WORK/libdragon"
find "$WORK/libdragon/tools" \( -name '*.o' -o -name '*.d' -o -name '*.a' \) -delete
make -C "$WORK/libdragon/tools" -j"$(getconf _NPROCESSORS_ONLN)" N64_INST="${N64_INST:-/opt/libdragon}" \
     mksprite mkasset audioconv64 mkdfs n64tool ed64romconfig >/dev/null
for t in mksprite/mksprite mkasset/mkasset audioconv64/audioconv64 mkdfs/mkdfs n64tool ed64romconfig; do
    cp "$WORK/libdragon/tools/$t" "$STAGE/tools/"
done
cc -O2 -o "$STAGE/tools/navgen" tools/navgen.c -lm
rm -rf "$WORK/libdragon"

echo "== Python $PY_VER ($TRIPLE) + numpy + Pillow + Tk"
PYTGZ=cpython-$PY_VER+$PY_REL-$TRIPLE-install_only_stripped.tar.gz   # (no debug symbols: 200 MB less)
curl -sSfL -o "$WORK/dl/$PYTGZ" "https://github.com/astral-sh/python-build-standalone/releases/download/$PY_REL/$PYTGZ"
tar -xzf "$WORK/dl/$PYTGZ" -C "$STAGE"            # -> python/
"$STAGE/python/bin/python3" -m pip install -q --no-cache-dir --disable-pip-version-check numpy==1.26.4 pillow
"$STAGE/python/bin/python3" -c "import numpy, PIL, tkinter; print('python ok', numpy.__version__)"
find "$STAGE/python" -name '__pycache__' -prune -exec rm -rf {} +
rm -rf "$STAGE/python/lib/python3.12/test" "$STAGE/python/lib/python3.12/idlelib" "$STAGE/python/lib/python3.12/ensurepip" \
       "$STAGE/python/lib/python3.12/lib2to3" "$STAGE/python/lib/python3.12/site-packages/pip" "$STAGE/python/lib/python3.12/config-"*
# the standalone Python carries its debug symbols (libpython alone is 200 MB): drop them
if [ $MAC = 1 ]; then
    find "$STAGE/python" -type f \( -name '*.so' -o -name '*.dylib' \) -exec strip -x {} \; 2>/dev/null || true
else
    find "$STAGE/python" -type f \( -name '*.so' -o -name '*.so.*' \) -exec strip --strip-debug {} \; 2>/dev/null || true
fi
"$STAGE/python/bin/python3" -c "import numpy, PIL.Image, tkinter" 

echo "== converters"
cp tools/*.py "$STAGE/hl64/tools/"
cp src/npc.c "$STAGE/hl64/src/"
cp assets/*.png assets/*.mp3 "$STAGE/hl64/assets/"
cp valve_sdk/LICENSE "$STAGE/licenses/valve-sdk-LICENSE" 2>/dev/null || true
cp "$STAGE/python/LICENSE"* "$STAGE/licenses/" 2>/dev/null || true
cp README.md "$STAGE/licenses/HL64-README.md"
echo "$VERSION" > "$STAGE/VERSION"

echo "== package ($VERSION)"
if [ $MAC = 1 ]; then
    mkdir -p "$APP/Contents/MacOS"
    cat > "$APP/Contents/MacOS/HL64Builder" <<'EOF'
#!/bin/bash
R="$(cd "$(dirname "$0")/../Resources" && pwd)"
exec "$R/python/bin/python3" "$R/hl64/tools/builder_gui.py" "$@"
EOF
    chmod +x "$APP/Contents/MacOS/HL64Builder"
    cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>HL64Builder</string>
  <key>CFBundleDisplayName</key><string>Half-Life 64 ROM builder</string>
  <key>CFBundleIdentifier</key><string>io.github.hl64.builder</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleExecutable</key><string>HL64Builder</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
EOF
    # Apple silicon runs only signed code: an ad-hoc signature for everything in the bundle
    if command -v codesign >/dev/null; then
        find "$APP" -type f \( -perm -u+x -o -name '*.so' -o -name '*.dylib' \) -exec codesign --force -s - {} \; 2>/dev/null || true
        codesign --force --deep -s - "$APP" || true
    fi
    rm -f "$OUT/HL64Builder-$TARGET.zip"
    (cd "$WORK" && ditto -c -k --keepParent HL64Builder.app "$OUT/HL64Builder-$TARGET.zip" 2>/dev/null || zip -qry "$OUT/HL64Builder-$TARGET.zip" HL64Builder.app)
    ls -la "$OUT/HL64Builder-$TARGET.zip"
else
    cat > "$STAGE/HL64Builder" <<'EOF'
#!/bin/bash
# Half-Life 64 ROM builder: run this (double-click it, or ./HL64Builder in a terminal)
R="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
exec "$R/python/bin/python3" "$R/hl64/tools/builder_gui.py" "$@"
EOF
    chmod +x "$STAGE/HL64Builder"
    cat > "$STAGE/README.txt" <<EOF
Half-Life 64 ROM builder ($VERSION) for Linux

Run ./HL64Builder (or double-click it). Choose your Half-Life folder (found through Steam when it
can be), where the ROM goes and, if you like, where the temporary files are kept, then press
Build ROM. The first build converts the game data and takes 30-90 minutes.

It needs a desktop (X11 or Wayland with XWayland). No Valve game data is included: the ROM is
made from your own copy of Half-Life, so keep it to yourself.
EOF
    rm -f "$OUT/HL64Builder-$TARGET.tar.gz"
    tar -C "$WORK" -czf "$OUT/HL64Builder-$TARGET.tar.gz" HL64Builder
    ls -la "$OUT/HL64Builder-$TARGET.tar.gz"
fi
