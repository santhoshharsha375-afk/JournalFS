#!/bin/bash
# demo.sh -- End-to-end demonstration of JournalFS (non-FUSE fallback).
set -e
cd "$(dirname "$0")/.."

IMG="${1:-journalfs.img}"
CLI="./build/journalfs"
FSCK="./build/fsck_journalfs"

echo "=== JournalFS demonstration ==="
echo ""

echo "1. Format the image"
./build/mkfs_journalfs "$IMG"

echo ""
echo "2. Create and write a file"
$CLI "$IMG" write /notes.txt "JournalFS is a crash-consistent filesystem."
$CLI "$IMG" read /notes.txt
echo ""

echo "3. Create a directory and a nested file"
$CLI "$IMG" mkdir /docs
$CLI "$IMG" write /docs/readme.txt "Write-ahead logging keeps metadata safe."
$CLI "$IMG" ls /docs

echo ""
echo "4. Inspect the journal"
./build/journal_inspect "$IMG" | tail -3

echo ""
echo "5. Run the consistency checker"
$FSCK "$IMG"

echo ""
echo "6. Run a crash-injection stage and recover"
JFS_CRASH_POINT=6 $CLI "$IMG" create /crashed.txt >/dev/null 2>&1 || true
echo "   (injected a crash during create)"
$CLI "$IMG" recover
$FSCK "$IMG"

echo ""
echo "=== Demonstration complete ==="
