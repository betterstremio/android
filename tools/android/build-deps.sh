#!/bin/bash
# The native libraries the Android build links against, per ABI, with the NDK (run in Linux or WSL):
#   SDL2 (libSDL2.so, and its Java sources for the Gradle project), mbedTLS (static), curl (libcurl.so over mbedTLS).
# Output: $AB/deps/<abi>/{include,lib}, $AB/deps/sdl-java. Sources are pinned and checked.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
AB="${AB:-$HOME/abuild}"; NDK="${NDK:-$HOME/android-sdk/ndk/27.2.12479018}"; API="${API:-24}"
ABIS="${ABIS:-arm64-v8a armeabi-v7a x86}"
SRC="$AB/src"; mkdir -p "$SRC"
fetch() { # url sha256 file
  [ -f "$SRC/$3" ] || curl -fsSL -o "$SRC/$3" "$1"
  if [ -z "$2" ]; then sha256sum "$SRC/$3"; return; fi
  echo "$2  $SRC/$3" | sha256sum -c - >/dev/null || { echo "checksum mismatch: $3"; sha256sum "$SRC/$3"; exit 1; }
}
SDL_V=2.30.9; MBED_V=3.6.2; CURL_V=8.11.0
fetch https://github.com/libsdl-org/SDL/releases/download/release-$SDL_V/SDL2-$SDL_V.tar.gz "${SDL_SHA:-24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4}" sdl.tgz
fetch https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBED_V/mbedtls-$MBED_V.tar.bz2 "${MBED_SHA:-8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca}" mbedtls.tbz
fetch https://curl.se/download/curl-$CURL_V.tar.xz "${CURL_SHA:-db59cf0d671ca6e7f5c2c5ec177084a33a79e04c97e71cf183a5cdea235054eb}" curl.txz
for p in sdl:sdl.tgz:SDL2-$SDL_V mbedtls:mbedtls.tbz:mbedtls-$MBED_V curl:curl.txz:curl-$CURL_V; do
  IFS=: read -r n f d <<<"$p"; [ -d "$SRC/$d" ] || tar -xf "$SRC/$f" -C "$SRC"; done
mkdir -p "$AB/deps"; rm -rf "$AB/deps/sdl-java"; cp -r "$SRC/SDL2-$SDL_V/android-project/app/src/main/java" "$AB/deps/sdl-java"
TCF="$NDK/build/cmake/android.toolchain.cmake"
for ABI in $ABIS; do
  O="$AB/deps/$ABI"; mkdir -p "$O"
  C=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="$TCF" -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-$API -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$O" -DCMAKE_POSITION_INDEPENDENT_CODE=ON)
  if [ ! -f "$O/lib/libSDL2.so" ]; then echo "== SDL2 $ABI"
    cmake -S "$SRC/SDL2-$SDL_V" -B "$AB/b/sdl-$ABI" "${C[@]}" -DSDL_STATIC=OFF -DSDL_SHARED=ON -DSDL_TEST=OFF >/dev/null
    ninja -C "$AB/b/sdl-$ABI" install >/dev/null; fi
  if [ ! -f "$O/lib/libmbedtls.a" ]; then echo "== mbedTLS $ABI"
    cmake -S "$SRC/mbedtls-$MBED_V" -B "$AB/b/mbed-$ABI" "${C[@]}" -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF >/dev/null
    ninja -C "$AB/b/mbed-$ABI" install >/dev/null; fi
  if [ ! -f "$O/lib/libcurl.so" ]; then echo "== curl $ABI"
    cmake -S "$SRC/curl-$CURL_V" -B "$AB/b/curl-$ABI" "${C[@]}" -DBUILD_SHARED_LIBS=ON -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF \
      -DCURL_USE_MBEDTLS=ON -DCURL_USE_OPENSSL=OFF -DMBEDTLS_INCLUDE_DIR="$O/include" -DMBEDTLS_LIBRARY="$O/lib/libmbedtls.a" -DMBEDX509_LIBRARY="$O/lib/libmbedx509.a" -DMBEDCRYPTO_LIBRARY="$O/lib/libmbedcrypto.a" \
      -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DUSE_NGHTTP2=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_LIBIDN2=OFF -DCURL_DISABLE_LDAP=ON -DENABLE_THREADED_RESOLVER=ON -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none >/dev/null
    ninja -C "$AB/b/curl-$ABI" install >/dev/null; fi
  ls "$O/lib" | tr '\n' ' '; echo
done
echo DEPS_OK
