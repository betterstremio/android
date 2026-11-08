#!/bin/bash
# tests/recs/run.sh [--real DIR] : builds the recommendation maths test and runs it
#   no argument: synthetic cases with checks
#   --real DIR:  ranks what fetch.py saved in DIR (python3 -I tests/recs/fetch.py DIR first)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; mkdir -p "$ROOT/build"
cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -O1 -g ${SB_ASAN:+-fsanitize=address} -I"$ROOT/src" -I"$ROOT/third_party/cjson" \
  "$ROOT/tests/recs/recs_test.c" "$ROOT/src/core/recs_core.c" "$ROOT/third_party/cjson/cJSON.c" -lm -o "$ROOT/build/recs_test"
"$ROOT/build/recs_test" "$@"
