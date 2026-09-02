#!/bin/bash
# Device-free tests for the standalone 4 KiB/QD1 evidence validator.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd -P)
exec python3 "$ROOT/tests/unit/test_validate_4k_thread_qd1_evidence.py"
