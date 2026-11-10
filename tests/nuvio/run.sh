#!/bin/bash
# tests/nuvio/run.sh: the Nuvio merge and the profile swap of the store, under AddressSanitizer. Needs gcc and SDL2's headers.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$HERE/../.."; O="${TMPDIR:-/tmp}/nuvio-test.$$"; mkdir -p "$O"
F="-std=c11 -D_GNU_SOURCE -g -O1 -w -fsanitize=address,undefined -I$ROOT/src -I$ROOT/third_party/cjson"
gcc $F "$HERE/merge_test.c" "$ROOT/src/core/nuvio_merge.c" "$ROOT/third_party/cjson/cJSON.c" -lm -o "$O/merge" || exit 1
gcc $F $(sdl2-config --cflags) "$HERE/store_test.c" "$ROOT/src/core/store.c" "$ROOT/third_party/cjson/cJSON.c" $(sdl2-config --libs) -lm -lpthread -o "$O/store" || exit 1
rc=0
echo "== merge"; ASAN_OPTIONS=detect_leaks=1 "$O/merge" || rc=1
echo "== store"; mkdir -p "$O/data"; ASAN_OPTIONS=detect_leaks=0 "$O/store" "$O/data" || rc=1
rm -rf "$O"; [ $rc = 0 ] && echo "ALL PASSED" || echo "FAILED"; exit $rc
