#!/bin/bash
# fuse_smoke.sh -- Manual FUSE mount smoke test (run inside Linux/WSL2).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK=$(mktemp -d /tmp/jfs_fuse_XXXX)
IMG="$WORK/jfs.img"
MNT="$WORK/mnt"
mkdir -p "$MNT"

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

"$ROOT/build/mkfs_journalfs" "$IMG" >/dev/null
"$ROOT/build/journalfs" "$IMG" "$MNT" -f >"$WORK/fuse.log" 2>&1 &
FUSE_PID=$!

# Wait for the mount to appear.
for i in $(seq 1 20); do
    if grep -q "$MNT" /proc/mounts 2>/dev/null; then
        break
    fi
    sleep 0.25
done

if ! grep -q "$MNT" /proc/mounts 2>/dev/null; then
    echo "FUSE mount failed:"
    cat "$WORK/fuse.log"
    exit 1
fi

echo "MOUNTED"
echo "hello_from_fuse" > "$MNT/hi.txt"
test "$(cat "$MNT/hi.txt")" = "hello_from_fuse"
mkdir "$MNT/sub"
echo "nested" > "$MNT/sub/x.txt"
test "$(cat "$MNT/sub/x.txt")" = "nested"
mv "$MNT/hi.txt" "$MNT/renamed.txt"
test -f "$MNT/renamed.txt"
test ! -f "$MNT/hi.txt"
rm "$MNT/renamed.txt"
rm -r "$MNT/sub"
ls -a "$MNT" | grep -q hi.txt && { echo "leftover after rm"; exit 1; } || true
echo "OPS OK"

fusermount3 -u "$MNT"
echo "UNMOUNTED"

"$ROOT/build/fsck_journalfs" "$IMG"
echo "FUSE SMOKE TEST PASSED"
