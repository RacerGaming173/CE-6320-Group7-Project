# Dynamic Memory Allocator Simulator (CE 6320, Group 7)

This project implements `malloc`, `free`, and `realloc` on a simulated heap and
compares free-block management policies by replaying real allocation traces
from [mimalloc-bench](https://github.com/daanx/mimalloc-bench).

## Directory structure

```
.
├── README.md
├── docs/                    project context, proposal, report drafts
├── src/                     simulator sources
│   └── heap.h, heap.cpp     simulated heap: sbrk-style growth, boundary tags,
│                            split/coalesce primitives, heap checker (HEAP_CHECK)
├── tests/                   dev-only unit tests, NOT called by run.sh
│   ├── test_heap.cpp        heap tests (unit, checker negative, random stress)
│   └── run_tests.sh         builds + runs tests at -O2 and with ASan/UBSan
├── traces/                  bundled workloads (input data for run.sh)
│   ├── *.trace.gz           one gzip'd text trace per benchmark
│   ├── stats.csv            statistics of the bundled (truncated) traces
│   └── stats_full.csv       statistics of the complete, untruncated runs
└── tools/                   dev-machine capture tooling, NOT called by run.sh
    ├── shim/alloctrace.c    LD_PRELOAD shim that logs malloc/free/realloc/...
    ├── convert/trace_convert.cpp  cleans raw traces, validates, computes stats
    ├── build_bench.sh       fetches + builds mimalloc-bench programs, redis, tools
    ├── capture.sh           runs each benchmark under the shim -> traces/*.trace.gz
    └── verify_traces.sh     strictly re-validates every bundled trace (portable)
```

## Running the tests

`run.sh` (the grading entry point) is not written yet. To test the heap on
Linux or WSL with g++ or clang++:

```bash
bash tests/run_tests.sh            # default 100000 random stress ops
bash tests/run_tests.sh 1000000    # longer stress run (release build only)
```

The tests use no allocation policy. They call the heap's primitives (`extend`,
`split`, `set_alloc`, `coalesce`) directly and run the heap checker after every
operation.

## Simulated heap layout

The heap is a `std::vector<uint8_t>` that only grows, through `heap_extend`
(the `sbrk` analogue). Blocks are named by the **offset of their header**.
Block offsets and sizes are multiples of 16, and the minimum block is 32 B
(header + `prev` + `next` + footer).

```
0        8        16                                    brk-8      brk
| pro hdr| pro ftr| block | block | ... | block |  epilogue hdr  |
```

- Every block has a header and a footer, each storing `size | flags`. Bit 0
  means allocated; bit 1 means the previous block is allocated. Bit 1 is kept
  accurate now so that footer elimination can be added later.
- The payload starts at `offset + 8`. A free block keeps its explicit-list
  links at `offset + 8` (`prev`) and `offset + 16` (`next`).
- `coalesce(off, remove_free)` is policy-independent. It calls back for each
  free neighbour so the policy can unlink it before it is absorbed.

## Trace format (parsing contract)

Each `traces/<name>.trace.gz` decompresses (`gzip -dc`) to plain ASCII text,
one operation per line:

```
a <id> <size>              malloc(size). Also calloc (size = n*elem) and memalign-family
r <old_id> <size> <new_id> realloc(old, size) returned new_id (new_id == old_id: in place)
f <id>                     free(id)
```

- `<id>`: the pointer value from the captured run in lowercase hex, without `0x`
  (e.g. `5641a2b3c4d0`). It is only an identifier. Map it to your heap offset
  with the allocation-map hash table. An ID can appear again after it is
  freed, because the original allocator reused the address.
- `<size>`: requested payload bytes in decimal. `a` may have size `0`
  (`malloc(0)`); `r` always has size > 0.
- Exactly one space separates fields, every line ends in `\n`, and the file
  has no comments, headers, or blank lines.

The validator (`trace_convert --check`) enforces these guarantees on every
bundled trace:

- every `f <id>` and every `r <old_id> ...` names a currently live id;
- every `a <id>` and every `r ... <new_id>` (with `new_id != old_id`) names an
  id that is **not** live;
- blocks still live at the end are never freed. The program leaked them, or
  the trace was truncated. The simulator should leave them allocated.

So any unknown-ID free the simulator sees means a bug in the simulator, not in
the trace. The shim records alignment requests (`posix_memalign` etc.) as
plain `a` and drops the alignment. None of the bundled workloads depend on it.

Minimal C++ reader:

```cpp
char op; std::string id, nid; uint64_t sz;
while (in >> op) {
    if (op == 'a')      { in >> id >> sz; /* malloc */ }
    else if (op == 'r') { in >> id >> sz >> nid; /* realloc */ }
    else /* 'f' */      { in >> id; /* free */ }
}
// ids: std::stoull(id, nullptr, 16)
```

## Bundled workloads

Each trace is the **first 2,000,000 operations** of the run (malloc-large is
complete). `stats_full.csv` has the statistics of the full runs.

| trace | program (args as in mimalloc-bench `bench.sh`) | bundled ops | full-run ops | size p50 / p99 / max (B) | peak live (bundled) | character |
|---|---|---|---|---|---|---|
| cfrac | `cfrac 17545186520507317056371138836327483792789528` | 2.0M | 183M | 18 / 44 / 2608 | 10.7 KB, 218 blocks | tiny bignum objects, rapid churn |
| espresso | `espresso largest.espresso` | 2.0M | 66.7M | 48 / 840 / 55072 | 352 KB, 3894 blocks | logic minimizer (EDA), mixed sizes, some realloc |
| alloc-test1 | `alloc-test 1` | 2.0M | 100M | 9 / 440 / 4 MiB | 7.9 MB, 131071 blocks | random-position, Pareto-like sizes; large live set |
| larson1 | `larson 5 8 1000 5000 100 4141 1` | 2.0M | 95.7M | 503 / 990 / 73728 | 2.7 MB, 5006 blocks | server simulation, uniform 8–1000 B |
| redis | `redis-server` driven by `redis-benchmark -r 1000000 -n 100000 -P 16 lpush a 1 2 3 4 5 lrange a 1 5` | 2.0M | 3.3M | 26 / 7999 / 324096 | 4.9 MB, 13819 blocks | real server; 27% of ops are realloc (mostly in place) |
| malloc-large | `malloc-large` | 4001 | 4001 | 16 MB / 26 MB / 26 MB | 417 MB, 22 blocks | 5–25 MiB blocks; heap extension and big coalesces |

Notes:
- **malloc-large needs about 420 MB of simulated heap** if the heap is a real
  `std::vector<uint8_t>`. The grader's machine needs that much free memory.
- The 2M-op prefix of cfrac and alloc-test1 is mostly program start-up and
  ramp-up (alloc-test1 fills its 131K-slot table, then churns). Re-capture with
  a larger `MAX_OPS` if a longer steady state is needed.
- barnes (also in mimalloc-bench) is not bundled. It makes 20 allocations up
  front and never frees, so it doesn't exercise free-block management.
- We ran the multithreaded benchmarks (alloc-test, larson) with 1 thread. The
  shim's lock serializes the few calls redis makes from background threads.

## How the traces were captured (reproducibility)

Captured on Ubuntu 24.04 (WSL2), gcc 13, glibc malloc, mimalloc-bench commit
`37e0c534e17d98a073448cc3626e726afd5be046`, redis 6.2.7 built with `MALLOC=libc`
(otherwise its bundled jemalloc would bypass the shim). The build needs
neither sudo nor cmake.

```bash
bash tools/build_bench.sh          # clone/build into ~/alloctrace (WORK=... to change)
bash tools/capture.sh              # all benchmarks -> traces/*.trace.gz + stats
MAX_OPS=5000000 bash tools/capture.sh espresso   # re-capture one with a longer prefix
bash tools/verify_traces.sh        # re-check bundled traces (any machine with g++)
```

Each trace goes through four steps:

1. `LD_PRELOAD=alloctrace.so <program>` writes a raw trace.
2. `trace_convert` cleans the raw trace and truncates it. Cleaning drops frees
   of pointers allocated before the shim loaded, rewrites `realloc` of an
   unknown pointer as `a`, and inserts a free if the allocator hands out a
   live id again.
3. `trace_convert --check` re-validates the result in strict mode.
4. `gzip -9` compresses it.

The cleaning step changed nothing on any bundled workload. It found 0 unknown
frees and made 0 repairs.

The shim hooks `malloc`, `calloc`, `realloc`, `free`, `posix_memalign`,
`aligned_alloc`, `memalign`, `valloc`, and `pvalloc`. C++ `new` and `delete`
reach these through libstdc++. The hooks avoid stdio. They write records to a
1 MiB buffer and flush it with `write(2)`. A thread-local guard prevents
recursion, and a static arena serves `dlsym`'s bootstrap allocations. The shim
holds one global mutex across the real call and the log write, so the trace
order matches the real heap order. Output goes to `$ALLOCTRACE_OUT.<pid>`.

## Workload sources and licenses

The benchmark programs come from mimalloc-bench's `bench/` directory, and each
carries its own license (see the files there). cfrac and espresso are from
Grunwald, Zorn, and Henderson (PLDI 1993); larson is from Larson and Krishnan
(ISMM 1998); alloc-test and malloc-large were contributed to mimalloc-bench;
redis is BSD-3-Clause.
