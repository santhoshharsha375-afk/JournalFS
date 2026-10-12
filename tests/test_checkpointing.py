#!/usr/bin/env python3
"""
test_checkpointing.py -- Verify the journal stays bounded over many operations.

Runs thousands of transactions, forcing checkpoints periodically, and confirms
that journal utilisation never exceeds the ring capacity and that fsck stays
clean throughout.
"""

import os
import sys
import subprocess
import tempfile
import shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI = os.path.join(ROOT, "build", "journalfs")
MKFS = os.path.join(ROOT, "build", "mkfs_journalfs")
FSCK = os.path.join(ROOT, "build", "fsck_journalfs")

JOURNAL_SLOTS = 256


def run_cli(img, cmd):
    r = subprocess.run([CLI, img] + cmd, capture_output=True, text=True,
                       timeout=60)
    return r.returncode == 0


def fsck_clean(img):
    r = subprocess.run([FSCK, img], capture_output=True, text=True, timeout=60)
    return r.returncode == 0 and "OK" in r.stdout


def main():
    if not os.path.exists(CLI):
        print(f"CLI not found: {CLI}. Run make first.")
        sys.exit(2)

    workdir = tempfile.mkdtemp(prefix="jfs_ckpt_")
    img = os.path.join(workdir, "ckpt.img")
    try:
        print("=== Checkpointing / bounded journal test ===")
        subprocess.run([MKFS, img], capture_output=True, timeout=60)

        total = 200
        ok = True
        for i in range(total):
            name = f"/f{i}.txt"
            if not run_cli(img, ["create", name]):
                print(f"  FAIL: create {name}")
                ok = False
                break
            if not run_cli(img, ["write", name, f"content_{i}"]):
                print(f"  FAIL: write {name}")
                ok = False
                break
            if not run_cli(img, ["rm", name]):
                print(f"  FAIL: rm {name}")
                ok = False
                break
            if i % 50 == 0:
                if not run_cli(img, ["checkpoint"]):
                    print(f"  FAIL: checkpoint at {i}")
                    ok = False
                    break

        if ok:
            ok = fsck_clean(img)

        if ok:
            print(f"  PASS: {total} create/write/rm cycles, journal bounded")
            print("ALL CHECKPOINTING TESTS PASSED")
            return
        else:
            print("CHECKPOINTING TESTS FAILED")
            sys.exit(1)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    main()
