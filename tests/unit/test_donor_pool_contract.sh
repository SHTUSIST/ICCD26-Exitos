#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
tmp=$(mktemp -d /tmp/exitos-donor-pool-test.XXXXXX)
trap 'rm -rf -- "$tmp"' EXIT

cc=${CC:-cc}
common=(-O0 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I"$repo/include")
"$cc" "${common[@]}" \
  -Dioctl=donor_test_ioctl -Dfstatvfs=donor_test_fstatvfs \
  -Dpthread_mutex_init=donor_test_mutex_init \
  -Dpthread_mutex_destroy=donor_test_mutex_destroy \
  -c "$repo/src/donor.c" -o "$tmp/donor.o"
"$cc" "${common[@]}" \
  -c "$repo/tests/unit/donor_pool_contract_unit.c" -o "$tmp/test.o"
"$cc" "$tmp/donor.o" "$tmp/test.o" -o "$tmp/test-donor-pool" -lpthread

"$tmp/test-donor-pool"
