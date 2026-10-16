#!/usr/bin/env bash
# Lays out the AppDir of Wabcut's AppImage from a Linux build (release workflow, .github/workflows/release.yml):
# the install tree under usr/ (Qt deployed into usr/lib, usr/plugins and usr/qml, found through usr/bin/qt.conf), the
# launcher entry and the icon at the top, and AppRun, which starts usr/bin/wabcut.
#
#   make-appdir.sh <build dir> <AppDir>
set -euo pipefail

if [ $# -ne 2 ]; then
    echo "usage: $0 <build dir> <AppDir>" >&2
    exit 2
fi
build="$1"
appdir="$2"

rm -rf "$appdir"
# --strip: the executables' debug information would make the AppImage several times larger.
cmake --install "$build" --prefix "$appdir/usr" --strip

cp "$appdir/usr/share/applications/wabcut.desktop" "$appdir/wabcut.desktop"
cp "$appdir/usr/share/icons/hicolor/256x256/apps/wabcut.png" "$appdir/wabcut.png"
ln -s wabcut.png "$appdir/.DirIcon"

cat > "$appdir/AppRun" <<'EOF'
#!/bin/sh
# Wabcut's AppImage entry point.
here="$(dirname "$(readlink -f "$0")")"
exec "$here/usr/bin/wabcut" "$@"
EOF
chmod +x "$appdir/AppRun"

if command -v desktop-file-validate > /dev/null; then
    desktop-file-validate "$appdir/wabcut.desktop"
fi
echo "AppDir: $appdir ($(du -sh "$appdir" | cut -f1))"
