#!/usr/bin/env bash
# Dev machine only (Linux/WSL); not called by run.sh.
# Builds the pinned mimalloc-bench programs, redis, the shim, and the converter
# into $WORK/bin. Needs gcc, g++, make, git, and curl (no cmake, no sudo).
#   WORK=~/alloctrace bash tools/build_bench.sh
set -euo pipefail
TOOLS="$(cd "$(dirname "$0")" && pwd)"
WORK="${WORK:-$HOME/alloctrace}"
MB_COMMIT=37e0c534e17d98a073448cc3626e726afd5be046   # mimalloc-bench master, 2026-11-06
REDIS_VER=6.2.7                                     # version pinned by mimalloc-bench
JOBS=$(nproc)

mkdir -p "$WORK/bin"
cd "$WORK"

if [ ! -d mimalloc-bench ]; then
  git clone -q https://github.com/daanx/mimalloc-bench.git
fi
git -C mimalloc-bench fetch -q --depth 1 origin "$MB_COMMIT" 2>/dev/null || true
git -C mimalloc-bench checkout -q "$MB_COMMIT"
B="$WORK/mimalloc-bench/bench"

# Flags and source lists below reproduce bench/CMakeLists.txt by hand.
CFLAGS="-O2 -w -Wno-implicit-function-declaration -Wno-implicit-int -Wno-int-conversion -Wno-return-mismatch -Wno-incompatible-pointer-types"

echo "[build] cfrac"
gcc $CFLAGS -std=gnu89 -DNOMEMOPT=1 -o bin/cfrac \
  $(printf "$B/cfrac/%s " cfrac.c pops.c pconst.c pio.c pabs.c pneg.c pcmp.c podd.c phalf.c \
      padd.c psub.c pmul.c pdivmod.c psqrt.c ppowmod.c atop.c ptoa.c itop.c utop.c ptou.c errorp.c \
      pfloat.c pidiv.c pimod.c picmp.c primes.c pcfrac.c pgcd.c) -lm

echo "[build] espresso"
gcc $CFLAGS -std=gnu89 -o bin/espresso \
  $(printf "$B/espresso/%s " cofactor.c cols.c compl.c contain.c cubestr.c cvrin.c cvrm.c cvrmisc.c \
      cvrout.c dominate.c equiv.c espresso.c essen.c exact.c expand.c gasp.c getopt.c gimpel.c \
      globals.c hack.c indep.c irred.c main.c map.c matrix.c mincov.c opo.c pair.c part.c primes.c \
      reduce.c rows.c set.c setc.c sharp.c sminterf.c solution.c sparse.c unate.c utility.c verify.c) -lm

echo "[build] alloc-test"
g++ $CFLAGS -std=c++17 -DBENCH=4 -o bin/alloc-test \
  "$B/alloc-test/test_common.cpp" "$B/alloc-test/allocator_tester.cpp" -lpthread

echo "[build] malloc-large"
g++ $CFLAGS -std=c++17 -o bin/malloc-large "$B/malloc-large/malloc-large.cpp" -lpthread

echo "[build] larson"
g++ $CFLAGS -std=c++17 -Wno-unused-result -DCPP=1 -o bin/larson "$B/larson/larson.cpp" -lpthread

if [ ! -x "$WORK/redis-$REDIS_VER/src/redis-server" ]; then
  echo "[build] redis $REDIS_VER (MALLOC=libc so calls reach the shim, not bundled jemalloc)"
  [ -d "redis-$REDIS_VER" ] || curl -fsSL "https://download.redis.io/releases/redis-$REDIS_VER.tar.gz" | tar xz
  make -s -C "redis-$REDIS_VER" -j"$JOBS" MALLOC=libc BUILD_TLS=no >/dev/null
fi

echo "[build] shim + converter"
gcc -O2 -fPIC -shared -o bin/alloctrace.so "$TOOLS/shim/alloctrace.c" -ldl -lpthread
g++ -std=c++17 -O2 -o bin/trace_convert "$TOOLS/convert/trace_convert.cpp"

echo "[build] done -> $WORK/bin"
ls -l "$WORK/bin"
