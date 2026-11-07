#!/bin/bash
# The app as Android's libmain.so (SDL's activity loads it), per ABI, with the NDK (Linux or WSL):
#   tools/android/build-native.sh [abi...]      default: arm64-v8a armeabi-v7a x86
# The same sources and third-party code as tools/build.sh, with src/platform/android. Needs build-deps.sh first.
# Output: $AB/out/<abi>/libmain.so (stripped) and libmain.dbg.so (symbols, for crash addresses).
set -euo pipefail
ROOT="${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}"
AB="${AB:-$HOME/abuild}"; NDK="${NDK:-$HOME/android-sdk/ndk/27.2.12479018}"; API="${API:-24}"
ABIS="${*:-arm64-v8a armeabi-v7a x86}"
APPID="${APPID:-tv.stremioorigin.app}"
NDKBIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
TP="$ROOT/third_party"; KEYS="${KEYS:-${ORIGIN_BUILD:-$HOME/origin-build}/keys.env}"
JOBS="${JOBS:-$(nproc)}"
for ABI in $ABIS; do
  case "$ABI" in arm64-v8a) TRIPLE=aarch64-linux-android;; armeabi-v7a) TRIPLE=armv7a-linux-androideabi;; x86) TRIPLE=i686-linux-android;; x86_64) TRIPLE=x86_64-linux-android;; *) echo "unknown ABI $ABI"; exit 1;; esac
  CC="$NDKBIN/clang --target=$TRIPLE$API"; CXX="$NDKBIN/clang++ --target=$TRIPLE$API"; AR="$NDKBIN/llvm-ar"
  D="$AB/deps/$ABI"; B="$AB/native/$ABI"; O="$AB/out/$ABI"; mkdir -p "$B/tp/ass" "$B/obj" "$O"
  OPT="-O2 -fno-strict-aliasing -ffunction-sections -fdata-sections -fPIC"
  [ "$ABI" = armeabi-v7a ] || [ "$ABI" = x86 ] && OPT="$OPT -D_FILE_OFFSET_BITS=64"
  CFLAGS="$OPT -std=c11 -D_GNU_SOURCE -DSB_ANDROID=1 '-DSB_APP_ID=\"$APPID\"' -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -I$ROOT/src -I$D/include -I$TP/freetype/include -I$TP/harfbuzz/src -I$TP/fribidi/lib -I$TP/config/fribidi -I$TP/cjson -I$TP/stb -I$TP/qrcodegen -I$TP/webp -I$TP/crypto -I$TP/libass -I$B"
  python3 - "$KEYS" "$B/sbkeys.h" <<'PY'
import os, sys, hashlib
src, out = sys.argv[1], sys.argv[2]; kv = {}
if os.path.isfile(src):
    for ln in open(src):
        ln = ln.strip()
        if ln and not ln.startswith('#') and '=' in ln: k, v = ln.split('=', 1); kv[k.strip()] = v.strip().strip('"\'')
def arr(name):
    v = kv.get(name, '').encode(); pad = hashlib.sha256(b'sb:' + name.encode()).digest()
    return 'static const unsigned char SBK_%s[] = { %s0 }, SBK_%s_PAD[32] = { %s };\n#define SBK_%s_LEN %d\n' % (name, ''.join('%d,' % (c ^ pad[i % 32]) for i, c in enumerate(v)), name, ','.join(str(x) for x in pad), name, len(v))
body = '/* made by tools/android/build-native.sh from the private keys file: never commit */\n' + arr('TRAKT_CLIENT_ID') + arr('TMDB_API_KEY')
if not os.path.isfile(out) or open(out).read() != body: open(out, 'w').write(body)
PY
  # ---- third-party, once per ABI (in parallel) ----
  if [ ! -f "$B/tp/libtp.a" ] || [ ! -f "$B/tp/libass.a" ]; then
    echo "== third-party $ABI"; rm -f "$B"/tp/*.o "$B"/tp/ass/*.o; TPQ="-w $OPT"
    FT="-DFT2_BUILD_LIBRARY '-DFT_CONFIG_MODULES_H=<ftmodule-sb.h>' -I$TP/config -I$TP/freetype/include"
    jobs=()
    for f in base/ftbase base/ftinit base/ftsystem base/ftdebug base/ftbitmap base/ftglyph base/ftmm base/ftbbox base/ftsynth base/ftstroke \
             gzip/ftgzip truetype/truetype sfnt/sfnt smooth/smooth autofit/autofit psnames/psnames pshinter/pshinter cff/cff psaux/psaux; do
      echo "$CC $TPQ $FT -c $TP/freetype/src/$f.c -o $B/tp/ft_$(basename $f).o"; done > "$B/tp/cmds"
    echo "$CXX -w $OPT -std=c++11 -fno-exceptions -fno-rtti -DHAVE_FREETYPE=1 -DHAVE_PTHREAD=1 -I$TP/freetype/include -I$TP/harfbuzz/src -c $TP/harfbuzz/src/harfbuzz.cc -o $B/tp/harfbuzz.o" >> "$B/tp/cmds"
    for f in "$TP"/fribidi/lib/fribidi*.c; do echo "$CC $TPQ -DHAVE_CONFIG_H -I$TP/config/fribidi -I$TP/fribidi/lib -c $f -o $B/tp/fb_$(basename "${f%.c}").o"; done >> "$B/tp/cmds"
    echo "$CC $TPQ -c $TP/cjson/cJSON.c -o $B/tp/cJSON.o" >> "$B/tp/cmds"
    echo "$CC $TPQ -c $TP/qrcodegen/qrcodegen.c -o $B/tp/qrcodegen.o" >> "$B/tp/cmds"
    for f in "$TP"/webp/src/dec/*.c "$TP"/webp/src/dsp/*.c "$TP"/webp/src/utils/*.c; do   # libwebp, decoder only; the SIMD files of this ABI stay
      n="$(basename "$f")"; case "$n" in *enc*|*writer*|*quant_levels_utils*|*_msa*|*_mips*) continue;; esac
      case "$ABI:$n" in x86*:*_neon*) continue;; arm*:*_sse*) continue;; esac
      echo "$CC $TPQ -DWEBP_REDUCE_CSP -I$TP/webp -c $f -o $B/tp/webp_${n%.c}.o"; done >> "$B/tp/cmds"
    for f in "$TP"/libass/libass/*.c "$TP"/libass/libass/c/*.c; do
      echo "$CC -w $OPT -std=gnu11 -DHAVE_CONFIG_H -I$TP/config/libass -I$TP/libass/libass -I$TP/freetype/include -I$TP/harfbuzz/src -I$TP/fribidi/lib -I$TP/config/fribidi -c $f -o $B/tp/ass/$(basename "${f%.c}").o"; done >> "$B/tp/cmds"
    echo "$CC -w $OPT -DFT2_BUILD_LIBRARY '-DFT_CONFIG_MODULES_H=<ftmodule-sb.h>' -I$TP/config -I$TP/freetype/include -c $TP/freetype/src/base/fttype1.c -o $B/tp/ass/ft_fttype1.o" >> "$B/tp/cmds"
    tr '\n' '\0' < "$B/tp/cmds" | xargs -0 -P "$JOBS" -I{} sh -c '{}' || { echo "THIRD-PARTY FAILED ($ABI)"; exit 1; }
    $AR rcs "$B/tp/libtp.a" "$B"/tp/*.o; $AR rcs "$B/tp/libass.a" "$B"/tp/ass/*.o
  fi
  # ---- the app ----
  echo "== app $ABI"
  : > "$B/obj/cmds"; OBJS=()
  NEWH="$(find "$ROOT/src" -name '*.h' -newer "$B/obj/.stamp" 2>/dev/null | head -1 || true)"; [ -f "$B/obj/.stamp" ] || NEWH=1
  for f in "$TP"/crypto/*.c "$ROOT"/src/core/*.c "$ROOT"/src/ui/*.c "$ROOT"/src/screens/*.c "$ROOT"/src/platform/android/*.c "$ROOT"/src/main.c; do
    [ -f "$f" ] || continue
    case "$f" in "$TP"/*) o="$B/obj/tp_$(basename "${f%.c}").o"; fl="-w $OPT -I$TP/crypto";; *) o="$B/obj/$(echo "${f#$ROOT/src/}" | tr / _).o"; fl="$CFLAGS";; esac
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ -n "$NEWH" ] || [ "$B/sbkeys.h" -nt "$o" ]; then echo "$CC $fl -c '$f' -o '$o' 2>&1 | grep -v '^$' || true; [ -f '$o' ]" >> "$B/obj/cmds"; rm -f "$o"; fi
    OBJS+=("$o")
  done
  if [ -s "$B/obj/cmds" ]; then tr '\n' '\0' < "$B/obj/cmds" | xargs -0 -P "$JOBS" -I{} sh -c '{}' || { echo "COMPILE FAILED ($ABI)"; exit 1; }; fi
  touch "$B/obj/.stamp"
  $CXX -shared -o "$O/libmain.dbg.so" "${OBJS[@]}" "$B/tp/libass.a" "$B/tp/libtp.a" -L"$D/lib" -lSDL2 -lcurl -lGLESv2 -lEGL -llog -landroid -lz -lm -ldl \
    -static-libstdc++ -Wl,--gc-sections -Wl,-z,max-page-size=16384 -Wl,--build-id=sha1 -Wl,--no-undefined
  "$NDKBIN/llvm-strip" --strip-unneeded -o "$O/libmain.so" "$O/libmain.dbg.so"
  cp "$D/lib/libSDL2.so" "$D/lib/libcurl.so" "$O/"; "$NDKBIN/llvm-strip" --strip-unneeded "$O/libcurl.so" "$O/libSDL2.so"
  [ -f "$AB/engine/$ABI/libsbengine.so" ] && cp "$AB/engine/$ABI/libsbengine.so" "$O/" || echo "warning: no P2P engine for $ABI (tools/build-engine.sh android-$ABI)"
  ls -l "$O" | awk 'NR>1 {printf "  %-22s %6.1f MB\n", $NF, $5/1048576}'
done
echo NATIVE_OK
