#!/bin/bash
# Compare current source line counts against the committed baseline so we
# can report the complexity reduction achieved by the scope-cut.
cd "$(dirname "$0")/.." || exit 1
FILES="src/main.c src/recovery.c src/fuse_ops.c src/filesystem.c
       src/fsck.c src/transaction.c include/journalfs.h include/filesystem.h"
base=0
now=0
printf '%-28s %6s %6s %6s\n' FILE BASE NOW SAVED
for f in $FILES; do
    b=$(git show HEAD:"$f" | wc -l)
    n=$(wc -l < "$f")
    base=$((base + b))
    now=$((now + n))
    printf '%-28s %6d %6d %6d\n' "$f" "$b" "$n" $((b - n))
done
printf '%-28s %6d %6d %6d\n' TOTAL "$base" "$now" $((base - now))
echo "reduction: $(( (base - now) * 100 / base ))%"
