#!/bin/bash
# Report, per source file: total lines and lines belonging to the leading
# block comment (header), so we can see how much bulk Tier1-6 can remove.
cd "$(dirname "$0")/.." || exit 1
printf '%-28s %6s %6s %6s\n' FILE TOTAL HEADER PCT
for f in src/*.c include/*.h; do
    total=$(wc -l < "$f")
    hdr=0
    started=0
    while IFS= read -r line; do
        if [ "$started" -eq 0 ]; then
            case "$line" in
                '/*'*) started=1; hdr=1 ;;
                ''|' '*) ;;
                *) break ;;
            esac
        else
            hdr=$((hdr + 1))
            case "$line" in
                *'*/'*) break ;;
            esac
        fi
    done < "$f"
    pct=$(( hdr * 100 / (total > 0 ? total : 1) ))
    printf '%-28s %6d %6d %5d%%\n' "$f" "$total" "$hdr" "$pct"
done
