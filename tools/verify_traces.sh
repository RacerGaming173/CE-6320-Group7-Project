#!/usr/bin/env bash
# Strictly re-validates every bundled traces/*.trace.gz. Needs only g++ (or
# clang++) and gzip, so it runs on any machine.
set -euo pipefail
cd "$(dirname "$0")/.."
CXX=$(command -v g++ || command -v clang++)
mkdir -p build
$CXX -std=c++17 -O2 -o build/trace_convert tools/convert/trace_convert.cpp
status=0
for f in traces/*.trace.gz; do
  name=$(basename "$f" .trace.gz)
  gzip -dc "$f" | ./build/trace_convert --check --name "$name" || status=1
done
exit $status
