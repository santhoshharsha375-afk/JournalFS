#!/bin/bash
# Find the largest file writes used by tests and benchmarks, so we know
# whether removing indirect-block support (48 KB / 12 direct blocks cap)
# would break anything.
cd "$(dirname "$0")/.." || exit 1
echo "=== tests/jfs_test.c write sizes ==="
grep -nE 'fs_write_data|write_data\(|JFS_BLOCK_SIZE|BLOCK_SIZE|\b[0-9]{5}\b' tests/jfs_test.c | head -30
echo
echo "=== benchmarks/jfs_bench.c write sizes ==="
grep -nE 'fs_write_data|write\(|BLOCK_SIZE|size_t size|int size' benchmarks/jfs_bench.c | head -30
echo
echo "=== any write larger than 12 blocks (49152) anywhere ==="
grep -rnE '[0-9]{5,}' tests/jfs_test.c benchmarks/jfs_bench.c | head -20
