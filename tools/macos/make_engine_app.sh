#!/usr/bin/env bash
# tools/macos/make_engine_app.sh: wrap the native GPU process (gpu-arm64/bb-gpu) in a minimal
# game app bundle.
#
# Why: macOS grants Game Mode (and Direct-ish fullscreen treatment) through LaunchServices — a
# running process is a "game" only if its executable lives in a registered bundle that declares
# the games category. bb-gpu is a bare binary spawned by the game process, so gamepolicyd never
# sees it (its log shows nothing at all). P.T.'s app is built exactly this way: the game engine
# is the bundle's executable. The binary's libraries resolve by absolute path (LC_RPATH into
# the checkout), so a copy inside the bundle loads them unchanged; build.sh re-runs this after
# every engine build so the copy never goes stale.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."

app=out/gpu-arm64/bb-gpu.app
bin=out/gpu-arm64/bb-gpu
[[ -x $bin ]] || { echo "STOP: $bin is missing (build the engine first)"; exit 1; }

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$bin" "$app/Contents/MacOS/bb-gpu"

# The game's own icon (the engine's window IS the game).
game=${BB_GAME_DIR:-$(defaults read io.github.bbport.mac gameFolder 2>/dev/null || true)}
icon=${game:+$game/sce_sys/icon0.png}
if [[ -n $icon && -f $icon ]]; then
    work=$(mktemp -d)/AppIcon.iconset
    mkdir -p "$work"
    ok=1
    for size in 16 32 128 256 512; do
        sips -z "$size" "$size" "$icon" --out "$work/icon_${size}x${size}.png" >/dev/null || ok=0
        if (( size * 2 <= 512 )); then
            sips -z $((size * 2)) $((size * 2)) "$icon" --out "$work/icon_${size}x${size}@2x.png" >/dev/null || ok=0
        fi
    done
    if [[ $ok == 1 ]]; then
        iconutil -c icns "$work" -o "$app/Contents/Resources/AppIcon.icns" || true
    fi
    rm -rf "$(dirname -- "$work")"
fi

cat > "$app/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key>
	<string>en</string>
	<key>CFBundleDisplayName</key>
	<string>Bloodborne PC Port</string>
	<key>CFBundleExecutable</key>
	<string>bb-gpu</string>
	<key>CFBundleIdentifier</key>
	<string>io.github.bbport.engine</string>
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

plutil -lint "$app/Contents/Info.plist" >/dev/null
codesign --force --sign - "$app"

# LaunchServices has to know it before the game daemon can find it.
lsregister=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
if [[ -x $lsregister ]]; then
    "$lsregister" -f "$PWD/$app" >/dev/null 2>&1 || true
fi
echo "engine app: $app (io.github.bbport.engine, games)"
