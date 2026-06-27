#!/usr/bin/env bash
set -euo pipefail

# Remove generated files that should not be committed or packaged in source releases.
rm -rf build build_* cmake-build-* CMakeFiles
find . -type d -name '__pycache__' -prune -exec rm -rf {} +
find . -type f \( \
    -name '*.pyc' -o \
    -name '*.pyo' -o \
    -name '*.o' -o \
    -name '*.a' -o \
    -name '*.so' -o \
    -name '*.so.*' -o \
    -name '*.log' -o \
    -name '*.ts' -o \
    -name '*.pcap' -o \
    -name 'core' -o \
    -name 'core.*' \
\) -delete

echo "NxFrame source tree cleanup complete."
