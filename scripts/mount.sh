#!/bin/bash
# mount.sh -- Mount a JournalFS image through FUSE (non-debug mode).
set -e
cd "$(dirname "$0")/.."

IMG="${1:-journalfs.img}"
MNT="${2:-./mnt}"

mkdir -p "$MNT"
./build/journalfs "$IMG" "$MNT" -o allow_other
echo "Mounted $IMG at $MNT"
