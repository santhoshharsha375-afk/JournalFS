#!/usr/bin/env bash
# Capture the three demo transcripts into screenshots/ as plain text.
# Run from the repo root:  bash scripts/capture_all.sh
cd "$(dirname "$0")/.." || exit 1
OUT=screenshots
mkdir -p "$OUT"

echo "== building =="
make clean >/dev/null 2>&1
make > "$OUT/00_build.txt" 2>&1
if grep -qiE "warning|error" "$OUT/00_build.txt"; then
    echo "BUILD HAS WARNINGS/ERRORS:"
    grep -iE "warning|error" "$OUT/00_build.txt" | head
else
    echo "build: 0 warnings"
fi

echo "== 1: make test =="
make test > "$OUT/01_make_test.txt" 2>&1
tail -3 "$OUT/01_make_test.txt"

echo "== 1b: FUSE demo =="
bash scripts/fuse_capture.sh > "$OUT/01b_fuse.txt" 2>&1
tail -6 "$OUT/01b_fuse.txt"

echo "== 2: journal / recover / fsck =="
bash scripts/journal_capture.sh > "$OUT/02_journal_recover_fsck.txt" 2>&1
tail -8 "$OUT/02_journal_recover_fsck.txt"

echo "== 3: seeded random =="
./build/jfs_test -s 42 random > "$OUT/03_random_seed42.txt" 2>&1
tail -5 "$OUT/03_random_seed42.txt"

echo "== 3b: crash-test =="
make crash-test > "$OUT/03b_crash_test.txt" 2>&1
tail -3 "$OUT/03b_crash_test.txt"

echo "== done =="
ls -l "$OUT"
