#!/bin/bash
# setup.sh -- Install build dependencies (Ubuntu / WSL2).
set -e

echo "=== JournalFS setup ==="

if command -v apt-get >/dev/null 2>&1; then
    sudo apt-get update -y
    sudo apt-get install -y build-essential pkg-config libfuse3-dev \
        python3 python3-matplotlib
    echo "Dependencies installed."
else
    echo "apt-get not found. Please install manually:"
    echo "  build-essential, pkg-config, libfuse3-dev, python3, python3-matplotlib"
fi

echo "Run 'make' to build JournalFS."
