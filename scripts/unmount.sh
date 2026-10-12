#!/bin/bash
# unmount.sh -- Unmount a JournalFS FUSE mount.
set -e
cd "$(dirname "$0")/.."

MNT="${1:-./mnt}"

if command -v fusermount3 >/dev/null 2>&1; then
    fusermount3 -u "$MNT"
elif command -v fusermount >/dev/null 2>&1; then
    fusermount -u "$MNT"
else
    echo "fusermount3 not found; unmount manually: umount $MNT"
    exit 1
fi
echo "Unmounted $MNT"
