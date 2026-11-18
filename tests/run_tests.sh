#!/usr/bin/env bash
# Dev-only: builds and runs the unit tests twice, once optimized and once with
# ASan/UBSan. Not called by run.sh.
set -euo pipefail
cd "$(dirname "$0")/.."

CXX=$(command -v g++ || command -v clang++ || true)
if [ -z "$CXX" ]; then echo "no g++ or clang++ found" >&2; exit 1; fi
mkdir -p build
FLAGS="-std=c++17 -Wall -Wextra -Werror -DHEAP_CHECK -Isrc"
SRCS="src/heap.cpp tests/test_heap.cpp"

echo "== release (-O2) =="
$CXX $FLAGS -O2 $SRCS -o build/test_heap
./build/test_heap "$@"

echo "== debug (-O0 -g, address + undefined sanitizers) =="
$CXX $FLAGS -O0 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined $SRCS -o build/test_heap_asan
./build/test_heap_asan 20000
