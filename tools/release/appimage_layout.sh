# Shared AppRun construction; metadata and optional runtime hooks stay in the title.
write_apprun() {
APP_RUN_EXTRA=""
[ ! -f "$REPO/tools/apprun-extra.sh" ] || APP_RUN_EXTRA=$(cat "$REPO/tools/apprun-extra.sh")
LIBRARY_ENV='export LD_LIBRARY_PATH="$HERE/usr/lib:${LD_LIBRARY_PATH:-}"'
[ "${ISOLATE_LIBRARY_PATH:-0}" != 1 ] || LIBRARY_ENV='export RECOMP_HOST_LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"'
cat > "$APPDIR/AppRun" <<EOF
#!/bin/sh
HERE="\$(dirname "\$(readlink -f "\$0")")"
$LIBRARY_ENV
# Steam Deck: read the built-in pad as a real gamepad instead of letting Steam's
# desktop layout retype it as keyboard (which otherwise sends Esc on B, etc.).
export SDL_JOYSTICK_HIDAPI_STEAM=1
export SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD=1
SELF="\${APPIMAGE:-\$0}"
ROMDIR="\$(dirname "\$(readlink -f "\$SELF")")"
$APP_RUN_EXTRA
# Seed/refresh the release-owned mod catalog beside the .AppImage. Directory
# trees get mkdir -p + cp of their CONTENTS (never a file where a dir belongs,
# never a dir where a file belongs); user files are left alone entirely.
if [ -d "\$HERE/usr/bin/mods" ] && [ -w "\$ROMDIR" ]; then
    mkdir -p "\$ROMDIR/mods" 2>/dev/null || true
    cp -a "\$HERE/usr/bin/mods/." "\$ROMDIR/mods/" 2>/dev/null || true
    chmod -R u+rwX "\$ROMDIR/mods" 2>/dev/null || true
fi
ROM=""
for ext in $ROM_EXTS; do
    [ "\$ext" = "none" ] && break
    for f in "\$ROMDIR"/*."\$ext"; do [ -e "\$f" ] && ROM="\$f" && break 2; done
done
cd "\$ROMDIR" 2>/dev/null || true
# Seed rom.cfg from a ROM dropped beside the .AppImage instead of passing it as
# argv[1]. Passing argv[1] counts as an explicit positional ROM and SKIPS the
# GUI launcher entirely, which left no way to reach Settings or the Mods page —
# and on a desktop with no zenity/kdialog/yad the launcher's "Browse ROM" button
# cannot open a picker either, so the user was stuck with no route in at all.
# Seeding the cache instead means the launcher opens with the ROM already
# resolved; ticking "Skip launcher on boot" then boots straight to the game off
# the same cached path. Never clobber a rom.cfg that already resolves.
if [ -n "\$ROM" ]; then
    cached=""
    [ -f "\$ROMDIR/rom.cfg" ] && cached="\$(head -n1 "\$ROMDIR/rom.cfg" 2>/dev/null | tr -d '\\r\\n')"
    if [ -z "\$cached" ] || [ ! -f "\$cached" ]; then
        [ -w "\$ROMDIR" ] && printf '%s\\n' "\$ROM" > "\$ROMDIR/rom.cfg" 2>/dev/null || true
    fi
fi
if [ "\$#" -eq 0 ]; then
    exec "\$HERE/usr/bin/$EXE" $EXTRA_ARGS
fi
exec "\$HERE/usr/bin/$EXE" "\$@"
EOF
chmod +x "$APPDIR/AppRun"
}
