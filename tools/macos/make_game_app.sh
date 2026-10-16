#!/usr/bin/env bash
# tools/macos/make_game_app.sh: build "Bloodborne PC Port.app" — the app whose binary BECOMES
# the game.
#
# The launcher (tools/macos/bb_game_launcher.swift) checks the game folder and the checkout and
# execs tools/macos/run.sh, which execs the game: no launcher window, no Dock app of its own, no
# supervisor — the process the Finder starts IS the game (P.T.'s shape). Only a failure shows a
# dialog. Needs Xcode or the Command Line Tools (swiftc).
#   BB_GAME_DIR=<game> bash tools/macos/make_game_app.sh
#   open out/BloodbornePC.app
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
repo=$PWD
app=out/BloodbornePC.app
work=out/game-launcher-build

[[ $(uname -s) == Darwin ]] || { echo "STOP: run this on the Mac" >&2; exit 1; }
command -v swiftc >/dev/null || { echo "STOP: swiftc missing: xcode-select --install" >&2; exit 1; }

echo "=== swiftc (release, $(uname -m))"
rm -rf "$app" "$work"
mkdir -p "$work" "$app/Contents/MacOS" "$app/Contents/Resources"
swiftc -O -o "$work/BloodbornePC" tools/macos/bb_game_launcher.swift -framework AppKit
cp "$work/BloodbornePC" "$app/Contents/MacOS/BloodbornePC"

cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleDisplayName</key>
    <string>Bloodborne PC Port</string>
    <key>CFBundleExecutable</key>
    <string>BloodbornePC</string>
    <key>CFBundleIdentifier</key>
    <string>io.github.bbport.mac</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>Bloodborne PC Port</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>0.6</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSApplicationCategoryType</key>
    <string>public.app-category.games</string>
    <key>LSMinimumSystemVersion</key>
    <string>14.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
EOF
plutil -insert BBRepoPath -string "$repo" "$app/Contents/Info.plist"

# The game's own icon, if the game folder is known.
game=${BB_GAME_DIR:-$(defaults read io.github.bbport.mac gameFolder 2>/dev/null || true)}
icon=${game:+$game/sce_sys/icon0.png}
if [[ -n $icon && -f $icon ]]; then
    iconset=$work/AppIcon.iconset
    mkdir -p "$iconset"
    ok=1
    for size in 16 32 128 256 512; do
        sips -z "$size" "$size" "$icon" --out "$iconset/icon_${size}x${size}.png" >/dev/null || ok=0
        if (( size * 2 <= 512 )); then
            sips -z $((size * 2)) $((size * 2)) "$icon" --out "$iconset/icon_${size}x${size}@2x.png" >/dev/null || ok=0
        fi
    done
    if [[ $ok == 1 ]]; then
        iconutil -c icns "$iconset" -o "$app/Contents/Resources/AppIcon.icns"
        plutil -insert CFBundleIconFile -string AppIcon "$app/Contents/Info.plist"
        echo "icon: $icon"
    fi
else
    echo "note: no icon made (set the game folder first; defaults write io.github.bbport.mac gameFolder …)"
fi

plutil -lint "$app/Contents/Info.plist" >/dev/null
codesign --force --sign - --deep "$app"
# LaunchServices has to know the game before the game daemon can find it.
lsregister=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
if [[ -x $lsregister ]]; then
    "$lsregister" -f "$PWD/$app" >/dev/null 2>&1 || true
fi
echo "game app: $app ($(lipo -archs "$app/Contents/MacOS/BloodbornePC"))"
echo "Open it with:  open $app"
