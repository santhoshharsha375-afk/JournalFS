#!/bin/bash
# format_image.sh -- Format a new JournalFS disk image.
set -e
cd "$(dirname "$0")/.."

IMG="${1:-journalfs.img}"
./build/mkfs_journalfs "$IMG"
echo "Formatted $IMG"
