#!/bin/bash
# Keep the measurement-only global-table build honest.  The production default
# is sharded; the A/B campaign compiles this same source with one macro so no
# unrelated source drift can be mistaken for a lock-sharding speedup.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd -P)
TMP=$(mktemp -d /tmp/exitos-table-baseline.XXXXXX)
trap 'rm -rf -- "$TMP"' EXIT

"${CC:-cc}" -O2 -g -Wall -Wextra -D_GNU_SOURCE \
    -DEXITOS_TABLE_GLOBAL_BASELINE \
    -I"$ROOT/include" -I"$ROOT/tests/harness" \
    "$ROOT/tests/unit/test_fdatasync_takeover.c" "$ROOT/libexitos.a" \
    -o "$TMP/test_fdatasync_global" -lpthread

"$TMP/test_fdatasync_global"
