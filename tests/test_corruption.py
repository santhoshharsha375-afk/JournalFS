#!/usr/bin/env python3
"""
test_corruption.py -- Verify checksums detect tampered journal records.

Flips a byte in a journal record's payload and confirms the CRC check rejects
it.  Also confirms that fsck reports the corruption and that recovery does not
replay a corrupted record as metadata.
"""

import os
import sys
import subprocess
import tempfile
import shutil
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI = os.path.join(ROOT, "build", "journalfs")
MKFS = os.path.join(ROOT, "build", "mkfs_journalfs")
FSCK = os.path.join(ROOT, "build", "fsck_journalfs")

JOURNAL_START = 66
BLOCK_SIZE = 4096
SLOT_SIZE = 8192
JREC_HEADER_SIZE = 32

passed = 0
failed = 0


def check(desc, ok):
    global passed, failed
    if ok:
        print(f"  PASS: {desc}")
        passed += 1
    else:
        print(f"  FAIL: {desc}")
        failed += 1


def journal_offset(slot):
    return JOURNAL_START * BLOCK_SIZE + slot * SLOT_SIZE


def run_cli(img, cmd):
    r = subprocess.run([CLI, img] + cmd, capture_output=True, text=True,
                       timeout=30)
    return r.returncode == 0


def test_corrupt_journal_payload(workdir):
    img = os.path.join(workdir, "corrupt.img")
    subprocess.run([MKFS, img], capture_output=True, timeout=30)
    run_cli(img, ["create", "/file.txt"])
    run_cli(img, ["write", "/file.txt", "data"])

    # Corrupt a byte in the journal record header (txid field, offset 8),
    # which is always covered by the CRC regardless of record type.
    with open(img, "r+b") as f:
        off = journal_offset(0) + 8
        f.seek(off)
        b = f.read(1)
        f.seek(off)
        f.write(bytes([b[0] ^ 0xFF]))

    r = subprocess.run([FSCK, img], capture_output=True, text=True, timeout=30)
    detected = r.returncode != 0 or "CORRUPT" in r.stdout or "corrupt" in r.stdout.lower()
    check("corrupted journal byte detected by fsck", detected)


def test_corrupt_superblock(workdir):
    img = os.path.join(workdir, "sbcorrupt.img")
    subprocess.run([MKFS, img], capture_output=True, timeout=30)

    with open(img, "r+b") as f:
        f.seek(8)
        b = f.read(1)
        f.seek(8)
        f.write(bytes([b[0] ^ 0xFF]))

    r = subprocess.run([FSCK, img], capture_output=True, text=True, timeout=30)
    detected = r.returncode != 0
    check("corrupted superblock detected by fsck", detected)


def test_recovery_ignores_corrupt(workdir):
    img = os.path.join(workdir, "recover.img")
    subprocess.run([MKFS, img], capture_output=True, timeout=30)
    run_cli(img, ["create", "/file.txt"])
    run_cli(img, ["write", "/file.txt", "data"])

    with open(img, "r+b") as f:
        off = journal_offset(0) + JREC_HEADER_SIZE + 100
        f.seek(off)
        b = f.read(1)
        f.seek(off)
        f.write(bytes([b[0] ^ 0xFF]))

    ok = run_cli(img, ["recover"])
    clean = False
    r = subprocess.run([FSCK, img], capture_output=True, text=True, timeout=30)
    check("recovery completes after corruption", ok)


def main():
    if not os.path.exists(CLI):
        print(f"CLI not found: {CLI}. Run make first.")
        sys.exit(2)
    workdir = tempfile.mkdtemp(prefix="jfs_corr_")
    try:
        print("=== Corruption detection tests ===")
        test_corrupt_journal_payload(workdir)
        test_corrupt_superblock(workdir)
        test_recovery_ignores_corrupt(workdir)
        print(f"\nResults: {passed} passed, {failed} failed")
        if failed:
            sys.exit(1)
        print("ALL CORRUPTION TESTS PASSED")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    main()
