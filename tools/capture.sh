#!/usr/bin/env bash
# Dev machine only (Linux/WSL); not called by run.sh. Run build_bench.sh first.
# Runs each benchmark under the shim, then cleans, truncates to MAX_OPS, and
# validates the trace, writing traces/<name>.trace.gz and the stats CSVs.
#   [MAX_OPS=N] bash tools/capture.sh [benchmark...]
set -euo pipefail
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${WORK:-$HOME/alloctrace}"
MAX_OPS="${MAX_OPS:-2000000}"
BIN="$WORK/bin"
B="$WORK/mimalloc-bench/bench"
RAW="$WORK/raw"
REDIS="$WORK/redis-6.2.7/src"
SHIM="$BIN/alloctrace.so"
OUT="$PROJ/traces"
mkdir -p "$RAW" "$OUT"

[ -x "$BIN/trace_convert" ] || { echo "run tools/build_bench.sh first" >&2; exit 1; }

# traced <name> <cmd...>: run cmd under the shim, leaving $RAW/<name>.raw.<pid>
traced() {
  local name=$1; shift
  rm -f "$RAW/$name".raw.*
  ALLOCTRACE_OUT="$RAW/$name.raw" LD_PRELOAD="$SHIM" "$@"
}

run_redis() {
  # As in mimalloc-bench's bench.sh: trace the server while an untraced
  # redis-benchmark drives it. Persistence is off so it never forks an RDB child.
  local port=6399 dir; dir=$(mktemp -d)
  rm -f "$RAW"/redis.raw.*
  (cd "$dir" && ALLOCTRACE_OUT="$RAW/redis.raw" LD_PRELOAD="$SHIM" \
     "$REDIS/redis-server" --port $port --save "" --appendonly no --daemonize no \
     --logfile "$dir/server.log") &
  local spid=$!
  for _ in $(seq 50); do "$REDIS/redis-cli" -p $port ping >/dev/null 2>&1 && break; sleep 0.2; done
  "$REDIS/redis-benchmark" -p $port -r 1000000 -n 100000 -q -P 16 lpush a 1 2 3 4 5 lrange a 1 5
  "$REDIS/redis-cli" -p $port flushall >/dev/null
  "$REDIS/redis-cli" -p $port shutdown nosave >/dev/null 2>&1 || true
  wait $spid || true
  rm -rf "$dir"
}

run_bench() {
  case "$1" in
    # args are exactly those used by mimalloc-bench's bench.sh (1 thread where applicable)
    cfrac)        traced cfrac "$BIN/cfrac" 17545186520507317056371138836327483792789528 ;;
    espresso)     traced espresso "$BIN/espresso" "$B/espresso/largest.espresso" ;;
    barnes)       traced barnes "$BIN/barnes" < "$B/barnes/input" ;;
    alloc-test1)  traced alloc-test1 "$BIN/alloc-test" 1 ;;
    malloc-large) traced malloc-large "$BIN/malloc-large" ;;
    larson1)      traced larson1 "$BIN/larson" 5 8 1000 5000 100 4141 1 ;;
    redis)        run_redis ;;
    *) echo "unknown benchmark $1" >&2; return 1 ;;
  esac
}

# barnes is excluded by default: its 20 up-front allocations are never freed,
# so it doesn't exercise free-block management.
ALL="cfrac espresso alloc-test1 malloc-large larson1 redis"
LIST="${*:-$ALL}"

if [ $# -eq 0 ]; then rm -f "$OUT/stats.csv" "$OUT/stats_full.csv"; fi

for name in $LIST; do
  echo "================ $name"
  start=$(date +%s)
  run_bench "$name" > "$RAW/$name.stdout" 2>&1 || { echo "  (program exited nonzero, see $RAW/$name.stdout)"; }
  echo "  run: $(( $(date +%s) - start ))s"

  # The main process's file is the largest; forked children's are tiny.
  raw=$(ls -S "$RAW/$name".raw.* | head -1)
  nfiles=$(ls "$RAW/$name".raw.* | wc -l)
  [ "$nfiles" -gt 1 ] && echo "  note: $nfiles raw files (forked children ignored), using $raw"

  # Delete this benchmark's old CSV rows so a re-run doesn't duplicate them.
  sed -i "/^$name,/d" "$OUT/stats_full.csv" 2>/dev/null || true
  "$BIN/trace_convert" --name "$name" --stats-csv "$OUT/stats_full.csv" < "$raw" > /dev/null

  sed -i "/^$name,/d" "$OUT/stats.csv" 2>/dev/null || true
  "$BIN/trace_convert" --name "$name" --max-ops "$MAX_OPS" < "$raw" 2>/dev/null > "$RAW/$name.trace"
  "$BIN/trace_convert" --check --name "$name" --stats-csv "$OUT/stats.csv" < "$RAW/$name.trace"
  # -n omits the timestamp, so identical traces give identical .gz files.
  gzip -9 -n -c "$RAW/$name.trace" > "$OUT/$name.trace.gz"
  echo "  -> traces/$name.trace.gz ($(du -h "$OUT/$name.trace.gz" | cut -f1))"
  rm -f "$RAW/$name".raw.* "$RAW/$name.trace"
done
echo "done. stats: traces/stats.csv (bundled), traces/stats_full.csv (full runs)"
