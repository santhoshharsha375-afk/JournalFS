#!/usr/bin/env bash
# FUSE demo transcript: mount, write, read back, unmount, fsck.
cd "$(dirname "$0")/.." || exit 1
WORK=$(mktemp -d /tmp/jfs_cap_XXXX)
IMG="$WORK/jfs.img"
MNT="$WORK/mnt"
mkdir -p "$MNT"
trap 'fusermount3 -u "$MNT" 2>/dev/null; rm -rf "$WORK"' EXIT

echo "\$ mkfs_journalfs jfs.img"
./build/mkfs_journalfs "$IMG"
echo
echo "\$ journalfs jfs.img mnt -f &"
./build/journalfs "$IMG" "$MNT" -f >/dev/null 2>&1 &
for i in $(seq 1 20); do
    grep -q "$MNT" /proc/mounts 2>/dev/null && break
    sleep 0.25
done
grep -q "$MNT" /proc/mounts && echo "MOUNTED" || { echo "MOUNT FAILED"; exit 1; }
echo
echo "\$ echo hello > mnt/hello.txt"
echo hello > "$MNT/hello.txt"
echo "\$ cat mnt/hello.txt"
cat "$MNT/hello.txt"
echo
echo "\$ ls -l mnt"
ls -l "$MNT"
echo
echo "\$ fusermount3 -u mnt"
fusermount3 -u "$MNT"
echo "UNMOUNTED"
echo
echo "\$ fsck_journalfs jfs.img"
./build/fsck_journalfs "$IMG"
