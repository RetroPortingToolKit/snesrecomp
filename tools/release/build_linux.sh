#!/usr/bin/env bash
# Shared Linux build and AppImage pipeline. Game metadata stays in tools/linux-release.sh.
set -euo pipefail
ENGINE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [ "${1:-}" != --project-root ] || [ "$#" -lt 2 ]; then
  echo "usage: $0 --project-root PROJECT [--config prod|debug] [--version VERSION] [--build DIR] [--out DIR] [--jobs N] [--regen] [--no-package]" >&2
  exit 2
fi
REPO="$(cd "$2" && pwd)"; shift 2
source "$REPO/tools/linux-release.sh"
# Pinned AppImage tooling (same pins as the Mega Man X / Tomba Linux releases).
LINUXDEPLOY_URL=https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage
LINUXDEPLOY_SHA=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
APPIMAGETOOL_URL=https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
APPIMAGETOOL_SHA=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0

CONFIG="prod"
DO_REGEN=0
DO_RUN=0
DO_PACKAGE=1
VERSION=""
PREBUILT=""; PREBUILT_SHA256=""; PREBUILT_RECEIPT=""
ENABLE_LUA=OFF; NOPIN=0; VARIANT="${SNES_RELEASE_VARIANT:-stock}"
JOBS="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
BUILD=""
OUT="$REPO/release-linux"

while [ $# -gt 0 ]; do
  case "$1" in
    --config) CONFIG="$2"; shift 2;;
    --prod) CONFIG="prod"; shift;;
    --debug) CONFIG="debug"; shift;;
    --version) VERSION="$2"; shift 2;;
    --regen) DO_REGEN=1; shift;;
    --prebuilt) PREBUILT="$2"; shift 2;;
    --prebuilt-sha256) PREBUILT_SHA256="$2"; shift 2;;
    --prebuilt-receipt) PREBUILT_RECEIPT="$2"; shift 2;;
    --lua) ENABLE_LUA=ON; shift;;
    --nopin) NOPIN=1; shift;;
    --stock) VARIANT=stock; shift;;
    --coop) VARIANT=coop; shift;;
    --run) DO_RUN=1; shift;;
    --no-package) DO_PACKAGE=0; shift;;
    --out) OUT="$2"; shift 2;;
    --build) BUILD="$2"; shift 2;;
    --jobs) JOBS="$2"; shift 2;;
    -h|--help) echo "Linux/AppImage build: --config prod|debug --version VERSION --build DIR --out DIR --jobs N --regen --no-package --run"; exit 0;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
if [ -n "$PREBUILT" ]; then
  [ -n "$PREBUILT_SHA256" ] && [ -n "$PREBUILT_RECEIPT" ] && [ "$DO_REGEN" = 0 ] || {
    echo "--prebuilt requires its hash and receipt, and cannot use --regen" >&2; exit 2;
  }
elif [ -n "$PREBUILT_SHA256" ] || [ -n "$PREBUILT_RECEIPT" ]; then
  echo "prebuilt identity options require --prebuilt" >&2; exit 2
fi
case "$CONFIG" in prod) FLAGS=( "${PROD_CMAKE_FLAGS[@]}" );; debug) FLAGS=( "${DEBUG_CMAKE_FLAGS[@]}" );;
  *) echo "--config must be prod or debug (got '$CONFIG')" >&2; exit 2;; esac


# Default the version to the checked-out tag so a release build can't ship
# stamped "dev" by accident; explicit --version always wins.
if [ -z "$VERSION" ]; then
  VERSION="$(git -C "$REPO" describe --tags --exact-match 2>/dev/null | sed 's/^v//' || true)"
  [ -n "$VERSION" ] || VERSION="dev"
fi
FLAGS+=( -DSNESRECOMP_BUILD_VERSION="$VERSION" -DSNESRECOMP_ENABLE_LUA="$ENABLE_LUA"
         -DSNESRECOMP_FRAME_IMPL=HLE -DSNESRECOMP_APU_TIME_IMPL=NATIVE
         -DSNESRECOMP_DSP_MIX_IMPL=NATIVE )
if declare -F configure_game >/dev/null; then configure_game; fi
if [ "$VARIANT" != stock ] && ! declare -F configure_game >/dev/null; then
  echo "this title does not support --$VARIANT" >&2; exit 2
fi
if [ "$ENABLE_LUA" = ON ] && [ "${SUPPORTS_LUA:-0}" != 1 ]; then
  echo "this title does not declare a Lua payload" >&2; exit 2
fi
if [ -n "${UNPINNED_CMAKE_OPTION:-}" ]; then
  [ "$NOPIN" = 1 ] && FLAGS+=( "-D$UNPINNED_CMAKE_OPTION=ON" ) || FLAGS+=( "-D$UNPINNED_CMAKE_OPTION=OFF" )
elif [ "$NOPIN" = 1 ]; then
  echo "--nopin is unsupported by this title" >&2; exit 2
fi

# SDL3 is the default; SNESRECOMP_SDL_BACKEND=SDL2 selects the compatibility
# package. Prefer the host package over any cross-platform dependency prefix.
SDL_BACKEND="${SNESRECOMP_SDL_BACKEND:-SDL3}"
SDL_CFG_DIR="$( { find /usr/lib /usr/lib64 /usr/local/lib -type d -path "*cmake/$SDL_BACKEND" 2>/dev/null || true; } | head -1 )"
FLAGS+=( -DSNESRECOMP_SDL_BACKEND="$SDL_BACKEND" )
[ -n "$SDL_CFG_DIR" ] && FLAGS+=( "-D${SDL_BACKEND}_DIR=$SDL_CFG_DIR" )

# Single cleanup hook: remove the AppDir scratch dir and restore generated files.
WORK=""; RAN_PREBUILD=0
cleanup() {
  [ -n "$WORK" ] && rm -rf "$WORK"
  # Restore the repo's gen state (re-run POSTBUILD) even if the build failed.
  [ "$RAN_PREBUILD" = "1" ] && [ -n "$POSTBUILD_CMD" ] && { echo "[postbuild] $POSTBUILD_CMD"; eval "$POSTBUILD_CMD" || true; }
  return 0   # never let the trap's last test override the script's real exit code
}
trap cleanup EXIT

BUILD="${BUILD:-$REPO/build-linux-$CONFIG}"
echo "==================== $APP_NAME ($CONFIG, v$VERSION) ===================="
cd "$REPO"

[ -f "$ENGINE_ROOT/runner/runner.cmake" ] || {
  echo "ERROR: snesrecomp is not initialized; run 'git submodule update --init --recursive' first." >&2
  exit 1
}
[ -f "$REPO/recomp-ui/recomp_ui.cmake" ] || {
  echo "ERROR: recomp-ui is not initialized; run 'git submodule update --init --recursive' first." >&2
  exit 1
}

if [ "$DO_REGEN" = "1" ] && [ -n "$REGEN_CMD" ]; then
  echo "[regen] $REGEN_CMD"
  eval "$REGEN_CMD"
fi

if [ -z "$PREBUILT" ] && [ -n "$PREBUILD_CMD" ]; then
  echo "[prebuild] $PREBUILD_CMD"
  RAN_PREBUILD=1
  eval "$PREBUILD_CMD"
fi

if [ -n "$PREBUILT" ]; then
  python3 "$ENGINE_ROOT/tools/release/verify_prebuilt.py" "$PREBUILT" "$PREBUILT_SHA256" "$PREBUILT_RECEIPT" "$VERSION" "$REPO" "$CMAKE_TARGET" "$CONFIG" "$ENABLE_LUA" "$SDL_BACKEND" "${REQUIRED_RASTER:-}"
else
  echo "[1/4] configure ($CONFIG: ${FLAGS[*]})"
  cmake -S "$REPO" -B "$BUILD" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DSNESRECOMP_ROOT="$ENGINE_ROOT" "${FLAGS[@]}"
  echo "[2/4] build ($CMAKE_TARGET, -j$JOBS)"
  cmake --build "$BUILD" --target "$CMAKE_TARGET" -j"$JOBS"
fi

# Locate the produced ELF by magic (the NTFS mount marks every file executable,
# so the exec bit is meaningless here).
BIN="$PREBUILT"
if [ -z "$BIN" ]; then
while IFS= read -r f; do
  if [ "$(basename "$f")" = "$CMAKE_TARGET" ] && file -b "$f" 2>/dev/null | grep -q "ELF.*executable"; then BIN="$f"; break; fi
done < <(find "$BUILD" -maxdepth 3 -type f)
fi
[ -n "$BIN" ] || { echo "ERROR: no ELF named '$CMAKE_TARGET' under $BUILD" >&2; exit 1; }
echo "      ELF: $BIN ($(du -h "$BIN" | cut -f1))"

if [ "$DO_PACKAGE" = "0" ]; then echo "      (--no-package) done."; exit 0; fi

echo "[3/4] package AppImage"
mkdir -p "$OUT"
EXE="$(basename "$BIN")"
SLUG="$(echo "$APP_NAME" | tr '[:upper:] ' '[:lower:]-' | tr -cd 'a-z0-9-')"
WORK="$(mktemp -d)"   # cleaned by the EXIT trap registered above
APPDIR="$WORK/AppDir"; mkdir -p "$APPDIR"

# Pinned tooling, fetched into the build tree and SHA-verified so packaging is
# reproducible regardless of what happens to live in ~/recomp-tools.
TOOLS_DIR="$BUILD/appimage-tools"
mkdir -p "$TOOLS_DIR"
fetch_tool() { # url sha dest
  local url="$1" sha="$2" dest="$3"
  if [ ! -f "$dest" ] || [ "$(sha256sum "$dest" | awk '{print $1}')" != "$sha" ]; then
    echo "      fetching $(basename "$dest")"
    curl -fL --retry 3 "$url" -o "$dest.tmp"
    printf '%s  %s\n' "$sha" "$dest.tmp" | sha256sum -c - >/dev/null
    mv "$dest.tmp" "$dest"
  fi
  chmod 0755 "$dest"
}
LINUXDEPLOY_BIN="$TOOLS_DIR/linuxdeploy-x86_64.AppImage"
APPIMAGETOOL_BIN="$TOOLS_DIR/appimagetool-x86_64.AppImage"
fetch_tool "$LINUXDEPLOY_URL" "$LINUXDEPLOY_SHA" "$LINUXDEPLOY_BIN"
fetch_tool "$APPIMAGETOOL_URL" "$APPIMAGETOOL_SHA" "$APPIMAGETOOL_BIN"
LINUXDEPLOY="$LINUXDEPLOY_BIN --appimage-extract-and-run"
APPIMAGETOOL="$APPIMAGETOOL_BIN --appimage-extract-and-run"

# Icon: real boxart when ImageMagick is available, flat placeholder otherwise.
ICON="$WORK/$SLUG.png"
IMAGE_TOOL=""
command -v magick >/dev/null 2>&1 && IMAGE_TOOL=magick
[ -z "$IMAGE_TOOL" ] && command -v convert >/dev/null 2>&1 && IMAGE_TOOL=convert
if [ -n "$IMAGE_TOOL" ] && [ -f "$REPO/$BOXART" ]; then
  "$IMAGE_TOOL" "$REPO/$BOXART" -resize 240x240 -background transparent \
      -gravity center -extent 256x256 "$ICON"
else
  python3 - "$ICON" "$SLUG" <<'PY'
import sys, zlib, struct, hashlib
out, slug = sys.argv[1], sys.argv[2]
h = hashlib.md5(slug.encode()).digest()
r, g, b = h[0] | 0x30, h[1] | 0x30, h[2] | 0x30
N = 256; row = bytes([0]) + bytes([r, g, b]) * N; raw = row * N
def chunk(t, d):
    c = t + d
    return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n"
png += chunk(b"IHDR", struct.pack(">IIBBBBB", N, N, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
open(out, "wb").write(png)
PY
fi

cat > "$WORK/$SLUG.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=$APP_NAME
Exec=$EXE
Icon=$SLUG
Categories=Game;
Terminal=false
EOF

$LINUXDEPLOY --appdir "$APPDIR" --executable "$BIN" \
    --desktop-file "$WORK/$SLUG.desktop" --icon-file "$ICON"

if [ "${BUNDLE_COMPILER_RUNTIME:-0}" = 1 ]; then
bundle_runtime_lib() { # soname
  local soname="$1" src=""
  src="$(LD_LIBRARY_PATH="$APPDIR/usr/lib" ldd "$BIN" |
      awk -v lib="$soname" '$1 == lib { print $3; exit }')"
  [ -n "$src" ] && [ -f "$src" ] || {
    echo "ERROR: runtime dependency not found for bundling: $soname" >&2
    exit 1
  }
  echo "      bundling runtime lib: $soname <- $src"
  cp -L "$src" "$APPDIR/usr/lib/$soname"
  chmod 0644 "$APPDIR/usr/lib/$soname"
}
bundle_runtime_lib libstdc++.so.6
bundle_runtime_lib libgcc_s.so.1

# Prune libraries the app cannot actually reach.
#
# linuxdeploy copies the whole transitive closure it sees on the BUILD host,
# including libraries pulled in only by host-side dependencies we do NOT bundle
# (libfreetype / libharfbuzz drag in glib, pcre2, png16, brotli, bz2,
# graphite2). Those copies are unreachable through the binary's own RUNPATH
# ($ORIGIN/../lib); the only way to make the loader prefer them is a global
# LD_LIBRARY_PATH, which is exactly what the AppRun below must NOT set -- it
# is inherited by every child process, so the host zenity/kdialog the launcher
# spawns for the ROM picker would load OUR glib against the host GTK and die.
# That is the "Browse For ROM does nothing" bug (beads-0fu.4), fixed in F-Zero
# 1.6.1 and ported here.
#
# The rule is general: bundle a library only if everything above it in the
# chain is bundled too. Resolve the closure with LD_LIBRARY_PATH unset -- which
# is precisely what the shipped AppRun gives the loader -- and delete anything
# in usr/lib the loader did not choose.
if [ -d "$APPDIR/usr/lib" ]; then
  KEEP="$WORK/appdir-keep.txt"
  env -u LD_LIBRARY_PATH ldd "$APPDIR/usr/bin/$EXE" \
    | awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^\//) print $i }' \
    | while read -r p; do readlink -f "$p" 2>/dev/null || true; done \
    | sort -u > "$KEEP"
  pruned=0
  for f in "$APPDIR"/usr/lib/*; do
    [ -e "$f" ] || continue
    real="$(readlink -f "$f")"
    if ! grep -qxF "$real" "$KEEP"; then
      echo "      prune unreachable bundled lib: $(basename "$f")"
      rm -f "$f"
      pruned=$((pruned + 1))
    fi
  done
  echo "      pruned $pruned unreachable lib(s) from usr/lib"
  # Whatever survived must be reachable without LD_LIBRARY_PATH, or the
  # AppImage would only work by poisoning its children's environment.
  missing="$(env -u LD_LIBRARY_PATH ldd "$APPDIR/usr/bin/$EXE" \
    | grep -c 'not found' || true)"
  [ "$missing" = "0" ] || {
    echo "ERROR: unresolved libraries without LD_LIBRARY_PATH" >&2
    env -u LD_LIBRARY_PATH ldd "$APPDIR/usr/bin/$EXE" \
      | grep 'not found' >&2
    exit 1
  }
fi

fi
# The ImGui pre-boot launcher loads fonts + images from assets/ next to the exe
# (SDL_GetBasePath resolves to usr/bin inside the AppImage). CMake's launcher
# POST_BUILD staged them beside the build ELF; carry them into the AppDir so the
# launcher renders inside the packaged AppImage.
[ -d "$(dirname "$BIN")/assets" ] || {
  echo "ERROR: recomp-ui launcher assets/ missing beside $BIN" >&2
  exit 1
}
echo "      staging launcher assets/ -> AppDir/usr/bin/assets"
cp -r "$(dirname "$BIN")/assets" "$APPDIR/usr/bin/assets"

if declare -F stage_game_payload >/dev/null; then stage_game_payload; fi

# Extra read-only payload (co-op IPS). Never user state.
for rel in "${EXTRA_PAYLOAD[@]}"; do
  [ -f "$REPO/$rel" ] || { echo "ERROR: payload missing: $REPO/$rel" >&2; exit 1; }
  echo "      staging payload       -> AppDir/usr/bin/$(basename "$rel")"
  cp "$REPO/$rel" "$APPDIR/usr/bin/$(basename "$rel")"
done

# The release-owned mod catalog, when the game ships one. mod_runtime resolves
# "mods" against the anchored cwd (next to the .AppImage), NOT the mount, so the
# AppRun seeds it beside the .AppImage at launch; here we just carry the payload
# and refuse to publish an AppImage whose Mods page would come up empty.
BUILT_MODS="$(dirname "$BIN")/mods"
for manifest in "${REQUIRED_MOD_MANIFESTS[@]}"; do
  [ -f "$BUILT_MODS/$manifest" ] || {
    echo "ERROR: built-in mod catalog missing: $BUILT_MODS/$manifest" >&2
    exit 1
  }
done
if [ -d "$BUILT_MODS" ]; then
  echo "      staging mod catalog   -> AppDir/usr/bin/mods"
  mkdir -p "$APPDIR/usr/bin/mods"
  for rel in "${MOD_CATALOG_DIRS[@]:-.}"; do
    if [ "$rel" = . ]; then cp -r "$BUILT_MODS/." "$APPDIR/usr/bin/mods/";
    else mkdir -p "$APPDIR/usr/bin/mods/$(dirname "$rel")"; cp -r "$BUILT_MODS/$rel" "$APPDIR/usr/bin/mods/$rel"; fi
  done
fi

# Custom AppRun. State policy: everything user-visible lives NEXT TO the
# .AppImage, exactly like the Windows zip keeps it next to the exe. The engine
# itself anchors there ($APPIMAGE is preferred over /proc/self/exe in
# host_paths.c), so this script must keep APPIMAGE exported. Here we:
#   * seed/refresh any RELEASE-OWNED mod catalog beside the .AppImage (an
#     AppImage update repairs/updates its bundled packages) without touching
#     user-installed packages, config.ini, keybinds.ini, rom.cfg, or saves/,
#   * read the controller natively on a Steam Deck,
#   * find the ROM next to the .AppImage and run from its folder.
rm -f "$APPDIR/AppRun"   # linuxdeploy leaves it a symlink to the real exe
. "$ENGINE_ROOT/tools/release/appimage_layout.sh"
write_apprun
if declare -F check_game_payload >/dev/null; then check_game_payload; fi

APP="$OUT/$RELEASE_SLUG-linux-$VERSION-x86_64.AppImage"
rm -f "$APP"
ARCH=x86_64 $APPIMAGETOOL "$APPDIR" "$APP"
chmod +x "$APP"
echo "      BUILT: $APP ($(du -h "$APP" | cut -f1))"

echo "[4/4] layout test (state next to the .AppImage, payload stays read-only)"
bash "$ENGINE_ROOT/tools/release/test_appimage_layout.sh" "$APPDIR" "$CMAKE_TARGET"

if declare -F test_game_package >/dev/null; then test_game_package; fi

sha256sum "$APP"

if [ "$DO_RUN" = "1" ]; then echo "[run] $APP"; "$APP" || true; fi
