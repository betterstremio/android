#!/bin/bash
# The Android APK (Linux or WSL), after tools/android/build-deps.sh and tools/build-engine.sh android-<abi>:
#   tools/android/build-apk.sh            release: arm64-v8a + armeabi-v7a  -> <appid>_<ver>_android.apk
#   tools/android/build-apk.sh --dev      dev build (SB_* test settings from the launch intent), + x86 for the emulator
# Every build is signed with the release key ($ORIGIN_BUILD/keys/android-release.jks, made on the first run): an
# update only installs over an APK signed with the same key, so that file must never be lost.
set -euo pipefail
ROOT="${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}"
AB="${AB:-$HOME/abuild}"; OB="${ORIGIN_BUILD:-$HOME/origin-build}"; SDK="${ANDROID_HOME:-$HOME/android-sdk}"
DEV=0; for a in "$@"; do [ "$a" = --dev ] && DEV=1; done
if [ $DEV = 1 ]; then ABIS="${ABIS:-arm64-v8a armeabi-v7a x86}"; else ABIS="${ABIS:-arm64-v8a armeabi-v7a}"; fi
APPINFO="${APPINFO:-$ROOT/pkg/appinfo.json}"   # APPINFO=other.json: another version (the updater test builds a newer one)
APPID=tv.stremioorigin.app; VER="$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['version'])" "$APPINFO")"

ROOT="$ROOT" bash <(sed 's/\r$//' "$ROOT/tools/android/build-native.sh") $ABIS

# ---- the Gradle project, staged outside the repo ----
G="$AB/gradle"; mkdir -p "$G"
rm -rf "$G/app/src" "$G/app/build/sdl-java"; mkdir -p "$G/app"
cp -r "$ROOT/android/settings.gradle" "$ROOT/android/build.gradle" "$ROOT/android/gradle.properties" "$G/"
cp "$ROOT/android/app/build.gradle" "$G/app/"; cp -r "$ROOT/android/app/src" "$G/app/"
find "$G" -maxdepth 1 -type f -name '*.gradle' -exec sed -i 's/\r$//' {} +
find "$G/app" -path "$G/app/build" -prune -o -type f \( -name '*.gradle' -o -name '*.java' -o -name '*.xml' -o -name '*.properties' \) -exec sed -i 's/\r$//' {} +
mkdir -p "$G/app/build"; cp -r "$AB/deps/sdl-java" "$G/app/build/sdl-java"
for ABI in $ABIS; do J="$G/app/src/main/jniLibs/$ABI"; mkdir -p "$J"; cp "$AB/out/$ABI"/{libmain.so,libSDL2.so,libcurl.so} "$J/"
  [ -f "$AB/out/$ABI/libsbengine.so" ] && cp "$AB/out/$ABI/libsbengine.so" "$J/"; done
A="$G/app/src/main/assets/app"; mkdir -p "$A"; cp -r "$ROOT/assets" "$A/assets"; cp "$APPINFO" "$A/appinfo.json"
find "$A/assets" -name '*.json' -exec sed -i 's/\r$//' {} +
# the launcher icon and the TV banner, drawn from the webOS icons
python3 - "$ROOT" "$G/app/src/main/res" <<'PY'
import sys, os
from PIL import Image
root, res = sys.argv[1], sys.argv[2]
icon = Image.open(os.path.join(root, 'pkg/largeIcon.png')).convert('RGBA')
for d, s in (('mdpi', 48), ('hdpi', 72), ('xhdpi', 96), ('xxhdpi', 144), ('xxxhdpi', 192)):
    os.makedirs(f'{res}/mipmap-{d}', exist_ok=True); icon.resize((s, s), Image.LANCZOS).save(f'{res}/mipmap-{d}/ic_launcher.png')
os.makedirs(f'{res}/drawable-xhdpi', exist_ok=True)
b = Image.new('RGBA', (320, 180), (12, 11, 17, 255))
logo = Image.open(os.path.join(root, 'assets/logo.png')).convert('RGBA'); k = min(260 / logo.width, 120 / logo.height); logo = logo.resize((max(1, int(logo.width * k)), max(1, int(logo.height * k))), Image.LANCZOS)
b.alpha_composite(logo, ((320 - logo.width) // 2, (180 - logo.height) // 2)); b.convert('RGB').save(f'{res}/drawable-xhdpi/banner.png')
PY

# ---- the signing key (once) ----
mkdir -p "$OB/keys"; KS="$OB/keys/android-release.jks"; KP="$OB/keys/android-release.pass"
if [ ! -f "$KS" ]; then
  head -c 24 /dev/urandom | base64 | tr -d '/+=' > "$KP"; chmod 600 "$KP"
  keytool -genkeypair -keystore "$KS" -storetype PKCS12 -alias stremioorigin -keyalg RSA -keysize 4096 -validity 36500 \
    -storepass "$(cat "$KP")" -keypass "$(cat "$KP")" -dname "CN=Stremio Origin" >/dev/null 2>&1
  echo "== new release key: $KS (back it up: updates need it)"
fi

# ---- Gradle ----
GV=8.9; GH="$AB/gradle-$GV"
if [ ! -x "$GH/bin/gradle" ]; then curl -fsSL -o "$AB/gradle.zip" "https://services.gradle.org/distributions/gradle-$GV-bin.zip"
  echo "d725d707bfabd4dfdc958c624003b3c80accc03f7037b5122c4b1d0ef15cecab  $AB/gradle.zip" | sha256sum -c - >/dev/null; (cd "$AB" && unzip -qo gradle.zip); fi
echo "sdk.dir=$SDK" > "$G/local.properties"
cd "$G"
SB_ABIS="$ABIS" SB_APPINFO="$APPINFO" SB_KEYSTORE="$KS" SB_KEYSTORE_PASS="$(cat "$KP")" \
  "$GH/bin/gradle" --no-daemon -q assembleRelease $([ $DEV = 1 ] && echo -Pdev) 2>&1 | grep -v "^$" | grep -vi "warning: \[options\]\|deprecat" || true
APK="$G/app/build/outputs/apk/release/app-release.apk"; [ -f "$APK" ] || { echo "APK FAILED"; exit 1; }
mkdir -p "$AB/apk"; SUF=""; [ $DEV = 1 ] && SUF=_dev; NAME="${APPID}_${VER}_android${SUF}.apk"; cp "$APK" "$AB/apk/$NAME"
"$SDK/build-tools/35.0.0/apksigner" verify "$AB/apk/$NAME" && echo "signed ok"
ls -l "$AB/apk/$NAME" | awk '{printf "%s  %.1f MB\n", $NF, $5/1048576}'; sha256sum "$AB/apk/$NAME" | cut -c1-64
echo APK_OK
