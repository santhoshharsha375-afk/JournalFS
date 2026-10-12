#!/bin/bash
# test_basic.sh -- Basic functional tests for JournalFS (non-FUSE fallback).
set -e

cd "$(dirname "$0")/.."
IMG=/tmp/jfs_basic.img
PASS=0
FAIL=0

check() {
    local desc="$1"; shift
    if "$@" >/dev/null 2>&1; then
        echo "PASS: $desc"
        PASS=$((PASS+1))
    else
        echo "FAIL: $desc"
        FAIL=$((FAIL+1))
    fi
}

BF=build/journalfs
MF=build/mkfs_journalfs
CF=build/fsck_journalfs

rm -f "$IMG"
$MF "$IMG" >/dev/null

echo "=== Basic operation tests ==="
check "create file" $BF "$IMG" create /a.txt
check "write file" $BF "$IMG" write /a.txt hello_journalfs
check "read file" $BF "$IMG" read /a.txt
check "stat file" $BF "$IMG" stat /a.txt
check "mkdir" $BF "$IMG" mkdir /d
check "write nested" $BF "$IMG" write /d/b.txt nested_data
check "ls root" $BF "$IMG" ls /
check "ls dir" $BF "$IMG" ls /d
check "rename" $BF "$IMG" mv /a.txt /d/a.txt
check "fsck clean" $CF "$IMG"
check "rm file" $BF "$IMG" rm /d/a.txt
check "rmdir" $BF "$IMG" rmdir /d
check "fsck after cleanup" $CF "$IMG"

echo ""
echo "Results: $PASS passed, $FAIL failed"
if [ "$FAIL" -gt 0 ]; then
    echo "TESTS FAILED"
    exit 1
fi
echo "ALL TESTS PASSED"
