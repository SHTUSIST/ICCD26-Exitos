#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
tmp=$(mktemp -d /tmp/exitos-donor-dirfd-test.XXXXXX)
trap 'rm -rf -- "$tmp"' EXIT

cc=${CC:-cc}
common=(-O0 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I"$repo/include")
"$cc" "${common[@]}" -Dfstatfs=donor_test_fstatfs \
  -Dcalloc=donor_test_calloc \
  -c "$repo/src/donor.c" -o "$tmp/donor.o"
"$cc" "${common[@]}" \
  -c "$repo/tests/unit/donor_dirfd_errno_unit.c" -o "$tmp/test.o"
"$cc" "$tmp/donor.o" "$tmp/test.o" -o "$tmp/test-donor-dirfd" -lpthread

"$tmp/test-donor-dirfd"
