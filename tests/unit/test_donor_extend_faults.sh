#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
tmp=$(mktemp -d /tmp/exitos-donor-extend-test.XXXXXX)
trap 'rm -rf -- "$tmp"' EXIT
cc=${CC:-cc}
common=(-O0 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I"$repo/include")

"$cc" "${common[@]}" \
  -Dioctl=donor_test_ioctl \
  -Drealloc=donor_test_realloc \
  -Dposix_memalign=donor_test_posix_memalign \
  -c "$repo/src/donor.c" -o "$tmp/donor.o"
"$cc" "${common[@]}" \
  -c "$repo/tests/unit/donor_extend_fault_unit.c" -o "$tmp/test.o"
"$cc" "$tmp/donor.o" "$tmp/test.o" -o "$tmp/test-donor-extend" -lpthread

"$tmp/test-donor-extend"
