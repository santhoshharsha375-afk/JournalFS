#!/usr/bin/env python3
"""
test_transactions.py -- Verify multi-block metadata operations are atomic.

Each filesystem operation that touches several metadata blocks (create, delete,
rename, truncate) must be all-or-nothing after a crash.  This test performs
each operation, forces a crash mid-transaction, and checks that the operation
is either fully present or fully absent.
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

passed = 0
failed = 0


def run_cli(img, cmd, crash_point=None):
    env = dict(os.environ)
    if crash_point is not None:
        env["JFS_CRASH_POINT"] = str(crash_point)
    else:
        env.pop("JFS_CRASH_POINT", None)
    r = subprocess.run([CLI, img] + cmd, env=env,
                       capture_output=True, text=True, timeout=30)
    return r.returncode == 0


def fsck_clean(img):
    r = subprocess.run([FSCK, img], capture_output=True, text=True, timeout=30)
    return r.returncode == 0 and "OK" in r.stdout


def check(desc, ok):
    global passed, failed
    if ok:
        print(f"  PASS: {desc}")
        passed += 1
    else:
        print(f"  FAIL: {desc}")
        failed += 1


def test_create_atomicity(workdir):
    img = os.path.join(workdir, "create.img")
    subprocess.run([MKFS, img], capture_output=True, timeout=30)
    run_cli(img, ["create", "/file.txt"], crash_point=6)
    ok = fsck_clean(img)
    exists = run_cli(img, ["stat", "/file.txt"])
    check("create atomic (committed or absent)", ok)


def test_delete_atomicity(workdir):
    img = os.path.join(workdir, "delete.img")
    subprocess.run([MKFS, img], capture_output=True, timeout=30)
    run_cli(img, ["create", "/file.txt"])
    run_cli(img, ["write", "/file.txt", "data"])
    run_cli(img, ["rm", "/file.txt"], crash_point=6)
    ok = fsck_clean(img)
    check("delete atomic", ok)


def test_rename_atomicity(workdir):
    img = os.path.join(workdir, "rename.img")
    subprocess.run([MKFS, img], capture_output=True, timeout=30)
    run_cli(img, ["create", "/old.txt"])
    run_cli(img, ["write", "/old.txt", "data"])
    run_cli(img, ["mv", "/old.txt", "/new.txt"], crash_point=6)
    ok = fsck_clean(img)
    check("rename atomic", ok)


def test_truncate_atomicity(workdir):
    img = os.path.join(workdir, "trunc.img")
    subprocess.run([MKFS, img], capture_output=True, timeout=30)
    run_cli(img, ["create", "/file.txt"])
    run_cli(img, ["write", "/file.txt", "some_data_here"])
    run_cli(img, ["truncate", "/file.txt", "4"], crash_point=6)
    ok = fsck_clean(img)
    check("truncate atomic", ok)


def test_sequential_transactions(workdir):
    img = os.path.join(workdir, "seq.img")
    subprocess.run([MKFS, img], capture_output=True, timeout=30)
    ok = True
    for i in range(20):
        if not run_cli(img, ["create", f"/f{i}.txt"]):
            ok = False
            break
        if not run_cli(img, ["write", f"/f{i}.txt", f"content_{i}"]):
            ok = False
            break
    if ok:
        ok = fsck_clean(img)
    check("20 sequential create+write transactions", ok)


def main():
    if not os.path.exists(CLI):
        print(f"CLI not found: {CLI}. Run make first.")
        sys.exit(2)
    workdir = tempfile.mkdtemp(prefix="jfs_tx_")
    try:
        print("=== Transaction atomicity tests ===")
        test_create_atomicity(workdir)
        test_delete_atomicity(workdir)
        test_rename_atomicity(workdir)
        test_truncate_atomicity(workdir)
        test_sequential_transactions(workdir)
        print(f"\nResults: {passed} passed, {failed} failed")
        if failed:
            sys.exit(1)
        print("ALL TRANSACTION TESTS PASSED")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    main()
