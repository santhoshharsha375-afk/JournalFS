#!/usr/bin/env bash
# Journal / recover / fsck transcript.
cd "$(dirname "$0")/.." || exit 1
WORK=$(mktemp -d /tmp/jfs_jr_XXXX)
IMG="$WORK/jfs.img"
trap 'rm -rf "$WORK"' EXIT

echo "\$ mkfs_journalfs jfs.img"
./build/mkfs_journalfs "$IMG"
echo
echo "\$ journalfs jfs.img create /a.txt"
./build/journalfs "$IMG" create /a.txt
echo "\$ journalfs jfs.img write /a.txt 'journaled data'"
./build/journalfs "$IMG" write /a.txt "journaled data"
echo
echo "\$ journalfs jfs.img journal      # the write-ahead log"
./build/journalfs "$IMG" journal
echo
echo "\$ journalfs jfs.img recover      # replay committed transactions"
./build/journalfs "$IMG" recover
echo
echo "\$ journal_inspect jfs.img"
./build/journal_inspect "$IMG"
echo
echo "\$ fsck_journalfs jfs.img"
./build/fsck_journalfs "$IMG"
