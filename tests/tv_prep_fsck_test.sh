#!/bin/sh
# Verify prep.sh accepts e2fsck's "errors corrected" status under set -e.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
HELPER=$(mktemp)
trap 'rm -f "$HELPER"' EXIT HUP INT TERM
awk '
  /^fsck_checked\(\) \{/ { copy = 1 }
  copy { print }
  copy && /^}/ { exit }
' "$ROOT/android/tv/prep/prep.sh" > "$HELPER"
[ -s "$HELPER" ] || { echo "fsck_checked helper not found" >&2; exit 1; }
. "$HELPER"
e2fsck() { return "$MOCK_E2FSCK_RC"; }

MOCK_E2FSCK_RC=0
fsck_checked -fy /dev/mock
[ "$FSCK_RC" -eq 0 ]
MOCK_E2FSCK_RC=1
fsck_checked -fy /dev/mock
[ "$FSCK_RC" -eq 1 ]
MOCK_E2FSCK_RC=2
if fsck_checked -fy /dev/mock; then
  echo "e2fsck status 2 should fail" >&2
  exit 1
fi
[ "$FSCK_RC" -eq 2 ]
echo "نجح: يقبل e2fsck الرمزين 0 و1 ويرفض 2"
