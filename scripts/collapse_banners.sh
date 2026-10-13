#!/bin/bash
# Collapse the 3-line section banners:
#   /* -------- */
#   /* Title    */
#   /* -------- */
# into a single-line /* Title */ comment.  Safe to run repeatedly.
cd "$(dirname "$0")/.." || exit 1
python3 - "$1" <<'PY' 2>/dev/null || perl -0pi -e 's{/\* -{10,} \*/\n/\* (.*?) \*/\n/\* -{10,} \*/\n}{/* $1 */\n}gs' "$1"
import re, sys
p = sys.argv[1]
s = open(p).read()
s2 = re.sub(r'/\* -{10,} \*/\n/\* (.*?) \*/\n/\* -{10,} \*/\n', r'/* \1 */\n', s)
open(p, 'w').write(s2)
PY
