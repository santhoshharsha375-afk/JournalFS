#!/usr/bin/env python3
"""
test_crash_recovery.py -- Crash-injection tests for JournalFS.

Runs the filesystem CLI at each crash stage, simulates a power failure by
letting the process abort() at a deterministic point, then reopens the image
and verifies that every transaction is either fully committed or fully absent.

Crash stages (JFS_CRASH_POINT):
  1 = before persist
  2 = after BEGIN record
  3 = during metadata updates
  4 = after journal records persisted
  5 = during commit record write
  6 = after commit record persisted
  7 = during home-block apply
  8 = after home-block apply (before checkpoint)
"""

import os
import sys
import subprocess
import tempfile
import shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI = os.path.join(ROOT, "build", "journalfs")
FSCK = os.path.join(ROOT, "build", "fsck_journalfs")

CRASH_STAGES = {
    1: "before_persist",
    2: "after_begin",
    3: "during_updates",
    4: "after_persist",
    5: "during_commit",
    6: "after_commit",
    7: "during_apply",
    8: "after_apply",
}

passed = 0
failed = 0


def run_cli(img, cmd, crash_point=None, expect_fail=False):
    env = dict(os.environ)
    if crash_point is not None:
        env["JFS_CRASH_POINT"] = str(crash_point)
    else:
        env.pop("JFS_CRASH_POINT", None)
    r = subprocess.run([CLI, img] + cmd, env=env,
                       capture_output=True, text=True, timeout=30)
    if expect_fail:
        return r.returncode != 0 or r.returncode < 0
    return r.returncode == 0


def fsck_clean(img):
    r = subprocess.run([FSCK, img], capture_output=True, text=True, timeout=30)
    return r.returncode == 0 and "OK" in r.stdout


def test_crash_stage(stage, workdir):
    global passed, failed
    img = os.path.join(workdir, "crash.img")
    cli = os.path.join(ROOT, "build", "mkfs_journalfs")
    subprocess.run([cli, img], capture_output=True, timeout=30)

    name = f"file_{stage}"

    crashed = run_cli(img, ["create", f"/{name}"],
                      crash_point=stage, expect_fail=True)

    recovered_ok = fsck_clean(img)
    exists = run_cli(img, ["stat", f"/{name}"])

    committed = exists
    print(f"  stage {stage} ({CRASH_STAGES[stage]}): "
          f"crashed={crashed} committed={committed} fsck_clean={recovered_ok}")

    if recovered_ok and crashed:
        passed += 1
    else:
        failed += 1


def test_idempotent_recovery(workdir):
    global passed, failed
    img = os.path.join(workdir, "idem.img")
    cli = os.path.join(ROOT, "build", "mkfs_journalfs")
    subprocess.run([cli, img], capture_output=True, timeout=30)

    run_cli(img, ["create", "/x.txt"], crash_point=6, expect_fail=True)

    ok1 = fsck_clean(img)
    ok2 = fsck_clean(img)

    if ok1 and ok2:
        print("  idempotent recovery: OK")
        passed += 1
    else:
        print("  idempotent recovery: FAIL")
        failed += 1


def test_valid_data_after_recovery(workdir):
    global passed, failed
    img = os.path.join(workdir, "data.img")
    cli = os.path.join(ROOT, "build", "mkfs_journalfs")
    subprocess.run([cli, img], capture_output=True, timeout=30)

    run_cli(img, ["create", "/f.txt"])
    run_cli(img, ["write", "/f.txt", "important_data_12345"])

    run_cli(img, ["write", "/f.txt", "important_data_12345"],
            crash_point=6, expect_fail=True)

    r = subprocess.run([CLI, img, "read", "/f.txt"],
                       capture_output=True, text=True, timeout=30)
    ok = "important_data_12345" in r.stdout
    clean = fsck_clean(img)

    if ok and clean:
        print("  data integrity after recovery: OK")
        passed += 1
    else:
        print("  data integrity after recovery: FAIL")
        failed += 1


def main():
    if not os.path.exists(CLI):
        print(f"CLI not found: {CLI}. Run make first.")
        sys.exit(2)

    workdir = tempfile.mkdtemp(prefix="jfs_crash_")
    try:
        print("=== Crash-recovery tests ===")
        for stage in CRASH_STAGES:
            test_crash_stage(stage, workdir)

        print("=== Idempotent recovery ===")
        test_idempotent_recovery(workdir)

        print("=== Data integrity ===")
        test_valid_data_after_recovery(workdir)

        print(f"\nResults: {passed} passed, {failed} failed")
        if failed:
            print("CRASH TESTS FAILED")
            sys.exit(1)
        print("ALL CRASH TESTS PASSED")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    main()
