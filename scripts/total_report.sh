#!/bin/bash
# Total source line counts: committed baseline vs current working tree,
# across every src/*.c and include/*.h file.
cd "$(dirname "$0")/.." || exit 1
base=0
now=0
for f in src/*.c include/*.h; do
    b=$(git show HEAD:"$f" 2>/dev/null | wc -l)
    n=$(wc -l < "$f")
    base=$((base + b))
    now=$((now + n))
done
echo "baseline: $base"
echo "current:  $now"
echo "saved:    $((base - now))"
echo "reduction: $(( (base - now) * 100 / base ))%"
