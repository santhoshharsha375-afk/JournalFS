#!/usr/bin/env bash
# JournalFS reviewer demo -- run on Ubuntu after `make`.
# Prints each input command and its output, then runs tests + crash + bench.
set -e
cd "$(dirname "$0")/.."
J=./build/journalfs
IMG=demo.img

echo "================================================================"
echo " DEMO 1: format an image (create the 'disk')"
echo "================================================================"
rm -f $IMG
./build/mkfs_journalfs $IMG

echo
echo "================================================================"
echo " DEMO 2: basic file operations via CLI (input -> output)"
echo "================================================================"
echo "\$ journalfs demo.img create /report.txt"
$J $IMG create /report.txt
echo "\$ journalfs demo.img write /report.txt 'Hello Reviewer - JournalFS works!'"
$J $IMG write /report.txt "Hello Reviewer - JournalFS works!"
echo "\$ journalfs demo.img read /report.txt"
$J $IMG read /report.txt
echo
echo "\$ journalfs demo.img ls /"
$J $IMG ls /
echo "\$ journalfs demo.img stat /report.txt"
$J $IMG stat /report.txt

echo
echo "================================================================"
echo " DEMO 3: directories, rename, truncate, delete"
echo "================================================================"
echo "\$ journalfs demo.img mkdir /data"
$J $IMG mkdir /data
echo "\$ journalfs demo.img write /data/note.txt 'inside a subdir'"
$J $IMG write /data/note.txt "inside a subdir"
echo "\$ journalfs demo.img ls /data"
$J $IMG ls /data
echo "\$ journalfs demo.img mv /report.txt /data/final.txt   (rename)"
$J $IMG mv /report.txt /data/final.txt
echo "\$ journalfs demo.img ls /data"
$J $IMG ls /data
echo "\$ journalfs demo.img truncate /data/final.txt 5"
$J $IMG truncate /data/final.txt 5
echo "\$ journalfs demo.img read /data/final.txt"
$J $IMG read /data/final.txt
echo
echo "\$ journalfs demo.img rm /data/final.txt"
$J $IMG rm /data/final.txt
echo "\$ journalfs demo.img rmdir /data"
$J $IMG rmdir /data
echo "\$ journalfs demo.img ls /"
$J $IMG ls /

echo
echo "================================================================"
echo " DEMO 4: the write-ahead log (inspect journal records)"
echo "================================================================"
echo "\$ journalfs demo.img journal"
$J $IMG journal

echo
echo "================================================================"
echo " DEMO 5: fsck (consistency checker)"
echo "================================================================"
echo "\$ journalfs demo.img fsck"
$J $IMG fsck

echo
echo "================================================================"
echo " DEMO 6: crash-injection tests (kill process at every WAL stage)"
echo "================================================================"
./build/jfs_test crash
echo
./build/jfs_test -s 42 random

echo
echo "================================================================"
echo " DEMO 7: full test suite (unit + integration + stress)"
echo "================================================================"
make test 2>&1 | tail -5

echo
echo "================================================================"
echo " DEMO 8: benchmark (journal vs no-journal) + graphs"
echo "================================================================"
make benchmark 2>&1 | grep -E "ops/s|Results"
make graphs 2>&1 | tail -5
echo "Graphs written to: results/*.svg  (open in any browser)"

echo
echo "================================================================"
echo " DEMO 9 (optional, native Linux only -- not /mnt/c): FUSE mount"
echo "================================================================"
echo "  mkdir -p ~/jfsmnt"
echo "  ./build/mkfs_journalfs ~/jfs.img"
echo "  ./build/journalfs ~/jfs.img ~/jfsmnt -f &    # -f = foreground"
echo "  echo hi > ~/jfsmnt/a.txt; cat ~/jfsmnt/a.txt; ls ~/jfsmnt"
echo "  fusermount3 -u ~/jfsmnt"
echo "Or just run:  bash tests/fuse_smoke.sh"
echo "================================================================"
echo " DEMO COMPLETE"
echo "================================================================"
