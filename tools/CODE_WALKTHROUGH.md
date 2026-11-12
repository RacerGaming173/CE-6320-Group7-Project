# Trace tooling: code walkthrough

This file explains every section of the trace-capture code in `tools/`. None
of it runs during grading. It produced the bundled `traces/*.trace.gz`, and it
lets anyone regenerate them.

## The pipeline at a glance

```
build_bench.sh ──► builds benchmark programs + alloctrace.so + trace_convert
                         │
capture.sh ──► for each benchmark:
                 LD_PRELOAD=alloctrace.so <program>   →  raw trace (text)
                 trace_convert                         →  full-run stats
                 trace_convert --max-ops 2000000       →  clean, truncated trace
                 trace_convert --check                 →  strict validation + stats
                 gzip -9                               →  traces/<name>.trace.gz
                         │
verify_traces.sh ──► re-checks every bundled .gz (works on any machine with g++)
```

| File | Language | Runs where | Job |
|---|---|---|---|
| [shim/alloctrace.c](shim/alloctrace.c) | C | inside the traced program | Records every heap call |
| [convert/trace_convert.cpp](convert/trace_convert.cpp) | C++17 | dev machine (and `verify_traces.sh`) | Cleans, truncates, validates, and summarizes traces |
| [build_bench.sh](build_bench.sh) | bash | dev machine (Linux/WSL) | Fetches and compiles everything |
| [capture.sh](capture.sh) | bash | dev machine (Linux/WSL) | Runs benchmarks and writes `traces/` |
| [verify_traces.sh](verify_traces.sh) | bash | anywhere | Re-validates the bundled traces |

---

## 1. `shim/alloctrace.c`: the recorder

This is a shared library that `LD_PRELOAD` loads into the traced program. It defines its own `malloc`, `free`
and the rest, so the program's calls (including C++ `new`/`delete`, which go
through `malloc`) land here first. Each hook calls glibc's real function and
appends one line to the trace.

| Lines | Section | Purpose |
|---|---|---|
| [1–14](shim/alloctrace.c#L1-L14) | Header comment, includes | Purpose and output file name. `_GNU_SOURCE` enables `RTLD_NEXT`. The build command is in `build_bench.sh`; the output format is in the README. |
| [16–24](shim/alloctrace.c#L16-L24) | Real-function pointers | Addresses of glibc's `malloc`, `free`, etc. The shim can't call `malloc()` by name, because that name now refers to the shim itself (infinite recursion). |
| [26–41](shim/alloctrace.c#L26-L41) | Bootstrap arena | `dlsym()`, used to find the real `malloc`, may itself call `malloc`/`calloc` before the real one is known. Those calls are served from a 64 KB static buffer (`boot_alloc`) and never logged. `is_boot()` keeps such pointers away from glibc's `free`, which would crash on them. |
| [43–50](shim/alloctrace.c#L43-L50) | Output state | `lock` (global mutex), `in_hook` (per-thread recursion guard), the trace file descriptor, the `finalised` flag, and a 1 MB output buffer. |
| [52–104](shim/alloctrace.c#L52-L104) | Writing records | No `printf`, because stdio allocates and would re-enter `malloc`. `put_dec`/`put_hex` format numbers by hand; `emit` builds a line in the buffer; `flush_buf` writes it out with the raw `write()` syscall (handling short writes and `EINTR`); `open_out` lazily opens `$ALLOCTRACE_OUT.<pid>`. Batching gives about one syscall per 40K ops. |
| [106–115](shim/alloctrace.c#L106-L115) | Fork handlers | A forked child inherits the parent's buffer. The handlers stop it from writing those records twice and make it open its own `.<childpid>` file. Taking the lock before `fork` keeps a half-written record from being copied. |
| [117–140](shim/alloctrace.c#L117-L140) | Startup and shutdown | `init` looks up the 9 real functions and registers the fork handlers. `ctor` runs it before `main`. `dtor` flushes at exit and sets `finalised`, so frees made later by other libraries' cleanup code are written immediately rather than lost. |
| [142–148](shim/alloctrace.c#L142-L148) | `ENTER`/`LEAVE` macros | Wrap every hook. The outermost call takes the lock and logs. Nested calls (libc allocating inside a hook) pass straight through unlogged. The lock is held across **both** the real call and the log write, so the file order equals the real heap order, even with threads (redis). |
| [150–240](shim/alloctrace.c#L150-L240) | The 9 hooks | `malloc` and `calloc` log `a`. `free` logs `f`. It skips NULL and bootstrap pointers and logs *before* the real free, so glibc can't hand the address out again before the line is written. `realloc` logs `r`, with special cases: `realloc(NULL,s)` logs `a`, `realloc(p,0)` logs `f`, and a failed realloc logs nothing. The memalign family logs `a` and drops the alignment. |

The shim does no cleanup or validation. Any bug in it runs
inside every allocation of the traced program, so it stays as small as
possible, and all the cleanup happens offline in `trace_convert`.

---

## 2. `convert/trace_convert.cpp`: cleaner, validator, statistics

The converter reads a trace on stdin and runs in one of two modes.

- **Convert (default).** Reads a *raw* shim trace, fixes problems, optionally
  truncates it, and writes a *clean* trace to stdout.
- **`--check`.** Reads a *clean* trace and exits with status 1 on the first
  violation. It writes no trace. This mode proves a bundled trace meets the
  format.

Both modes print statistics to stderr and can append a row to a CSV.

The central data structure is the `live` map. It maps the id of every block
currently allocated to that block's requested size. The simulator's
allocation-map hash table does the same bookkeeping. Every rule below checks
this map.

| Lines | Section | Purpose |
|---|---|---|
| [1–12](convert/trace_convert.cpp#L1-L12) | Header comment, includes | Purpose and usage. The **parsing guarantees** of a clean trace (the contract the simulator relies on) are in the README. Each cleaning rule is commented at the branch that applies it. |
| [14–22](convert/trace_convert.cpp#L14-L22) | `struct Stats` | Counters: lines in and ops out, per-op counts, repairs made, current/peak live bytes and blocks, size min/max/total, and `sizes` (every request size, kept for percentiles). |
| [24–28](convert/trace_convert.cpp#L24-L28) | Globals | `live` (the map from id to size), the stats, the mode flag, and a 1 MB stdout buffer for speed. |
| [30–40](convert/trace_convert.cpp#L30-L40) | `note_size`, `bump_live` | Update size statistics, and live bytes/blocks along with their peaks. "Peak live bytes" is the numerator of the utilization metric. |
| [42–60](convert/trace_convert.cpp#L42-L60) | `out_a`, `out_f`, `out_r` | These are the only places that accept an op. Each writes the line (unless in `--check`), updates `live`, and updates stats. `out_r` handles the size change and the move from old id to new id, and counts in-place reallocs (`new == old`). |
| [62–78](convert/trace_convert.cpp#L62-L78) | Parsers | `parse_hex` (lowercase hex, at most 16 digits), `parse_dec`, and `sp` (exactly one space). They are hand-written instead of using `scanf` because they run faster on 100M-line inputs and reject `0x`, uppercase hex, tabs, and double spaces. |
| [80–83](convert/trace_convert.cpp#L80-L83) | `fail` | Prints the line number, the reason and the offending line in `--check` mode. |
| [85–94](convert/trace_convert.cpp#L85-L94) | Argument parsing | `--check`, `--max-ops N`, `--name`, `--stats-csv FILE`. |
| [95–138](convert/trace_convert.cpp#L95-L138) | Main loop | Sets up the output buffer and pre-sizes `live`, then reads one line at a time until EOF or `max_ops`. Malformed lines are skipped (convert) or fatal (check). Then, per op: |
| | &nbsp;&nbsp;`a id` | If `id` is already live, the shim missed a free, so emit a synthetic `f id` first. `--check` treats this as an error. |
| | &nbsp;&nbsp;`f id` | If `id` isn't live, the program allocated the block before the shim loaded, so drop the line. `--check` treats this as an error. |
| | &nbsp;&nbsp;`r old sz new` | If `old` is unknown, rewrite the line as `a new sz`. If `new` is already live and differs from `old`, emit a synthetic free first. `--check` treats both cases, and size 0, as errors. |
| [139–150](convert/trace_convert.cpp#L139-L150) | Wrap-up | Detects truncation (input left over after `max_ops`), sorts `sizes`, and defines `pct(q)` for p50/p90/p99/p99.9. |
| [151–162](convert/trace_convert.cpp#L151-L162) | Human-readable report | The `ops=… sizes: … peak live: … cleaning: …` block printed for each trace, plus `CHECK OK`. |
| [164–180](convert/trace_convert.cpp#L164-L180) | CSV output | Appends one row to `--stats-csv`, writing the header only if the file is empty. This produces `traces/stats.csv` and `stats_full.csv`. |

The cleaning rules exist because the shim loads slightly after the C runtime
starts, so the program may allocate a few blocks before the shim is watching.
Their later `free` or `realloc` calls would look like "unknown id" errors.
Every repair counter came out 0 on all bundled workloads, so the rules never
had to fix anything.

---

## 3. `build_bench.sh`: build everything

Run once on a Linux/WSL dev machine. It needs only gcc, g++, make, git, and
curl. It does not need cmake or sudo.

| Lines | Section | Purpose |
|---|---|---|
| [1–14](build_bench.sh#L1-L14) | Settings | `set -euo pipefail` (stop on any error), `TOOLS` (this script's directory), `WORK` (build area, default `~/alloctrace`), the pinned mimalloc-bench commit and redis version, and the job count. |
| [16–24](build_bench.sh#L16-L24) | Fetch mimalloc-bench | Clones the repo if absent and checks out the **pinned commit**, so traces are reproducible even if upstream changes. Sets `CFLAGS` to the same warning-suppression flags mimalloc-bench's CMake uses (these are old K&R-style C programs). |
| [26–47](build_bench.sh#L26-L47) | Benchmark programs | One `gcc`/`g++` line per program, reproducing `bench/CMakeLists.txt` by hand: the same source lists, `-std=gnu89` for the old C code, and the same defines (`NOMEMOPT=1` for cfrac, `BENCH=4` for alloc-test, `CPP=1` for larson). `printf "$B/dir/%s "` expands a list of file names into full paths. |
| [49–53](build_bench.sh#L49-L53) | redis | Downloads redis 6.2.7 and builds it with **`MALLOC=libc`**. By default redis uses its bundled jemalloc, which never calls glibc `malloc`, so the shim would see nothing. Skipped if already built. |
| [55–57](build_bench.sh#L55-L57) | Our tools | Builds the shim as a shared library (`-fPIC -shared`, linked with `-ldl` for `dlsym`) and the converter. |
| [59–60](build_bench.sh#L59-L60) | Done | Lists `$WORK/bin`. |

---

## 4. `capture.sh`: run the benchmarks and produce `traces/`

| Lines | Section | Purpose |
|---|---|---|
| [1–18](capture.sh#L1-L18) | Settings | Paths (`PROJ` = project root, found from the script's location), `MAX_OPS` (default 2,000,000), and a check that `build_bench.sh` has been run. |
| [21–25](capture.sh#L21-L25) | `traced NAME CMD…` | Deletes old raw files, then runs the command with `LD_PRELOAD` and `ALLOCTRACE_OUT` set. |
| [27–42](capture.sh#L27-L42) | `run_redis` | Redis is a server, so it can't run as a single command. The function starts `redis-server` under the shim in the background with persistence off (`--save ""`, `--appendonly no`), so the server never forks. It waits until `redis-cli ping` succeeds, then runs the **untraced** `redis-benchmark` client with mimalloc-bench's exact arguments. Finally it sends `flushall` and `shutdown`, so the server exits normally and the shim's destructor flushes the buffer. |
| [44–56](capture.sh#L44-L56) | `run_bench NAME` | Maps each benchmark name to the exact command line from mimalloc-bench's `bench.sh`, with 1 thread for alloc-test and larson. barnes stays here but isn't in the default list. |
| [58–63](capture.sh#L58-L63) | Benchmark list | `ALL` is the default set (the comment above it explains why barnes is left out). Command-line arguments select a subset (`bash capture.sh cfrac redis`). A full run starts with fresh stats CSVs. |
| [65–69](capture.sh#L65-L69) | Run | Runs the benchmark, sends its stdout to a log, and times it. If the benchmark exits nonzero, the script reports it and keeps going. |
| [71–74](capture.sh#L71-L74) | Pick the raw file | If the program forked, there are several `.raw.<pid>` files. The largest one belongs to the main process. |
| [76–78](capture.sh#L76-L78) | Full-run stats | Converts the whole raw trace, discarding the output, to record untruncated statistics in `stats_full.csv`. The `sed` deletes this benchmark's old row first, so re-running one benchmark doesn't create duplicates. |
| [80–87](capture.sh#L80-L87) | Bundled trace | Convert with `--max-ops`, then **re-validate the result with `--check`** (this run writes `stats.csv`), then `gzip -9 -n` (`-n` omits the timestamp, so identical traces give identical `.gz` files). The script then deletes the raw files, which can exceed 1 GB each. |

---

## 5. `verify_traces.sh`: re-check the bundled traces

This script is short and portable. It needs only g++ (or clang++) and gzip, so a teammate or
grader on any machine can confirm the traces are intact.

| Lines | Section | Purpose |
|---|---|---|
| [1–8](verify_traces.sh#L1-L8) | Setup | `cd` to the project root, pick a compiler, build `trace_convert` into `build/`. |
| [9–14](verify_traces.sh#L9-L14) | Loop | Decompress each `traces/*.trace.gz` into `trace_convert --check`. Exits nonzero if any trace fails. |
