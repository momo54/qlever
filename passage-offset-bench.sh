#!/usr/bin/env bash
#
# Reproduce the `OFFSET` block-skipping measurement of `PASSAGE-README.md`
# step 6, end to end: build, generate data, build the index, then time the same
# queries with and without the skipping and print the table.
#
# The "without" half needs the skipping neutralised, which no runtime flag can
# do, so this applies `passage-no-offset-skip.patch`, rebuilds `qlever-server`,
# measures, and reverts. The patch is reverted and the binary rebuilt on every
# exit path, including Ctrl-C; the script refuses to start if `src/` has
# uncommitted changes, so that a failed revert cannot be mistaken for your own
# work.
#
# Usage:  ./passage-offset-bench.sh [triples] [workdir]
#         ./passage-offset-bench.sh                  # 20 M, ./passage-bench-work
#         ./passage-offset-bench.sh 2000000          # the smaller size
#
set -euo pipefail

TRIPLES=${1:-20000000}
WORKDIR=${2:-$PWD/passage-bench-work}
PORT_SKIP=${PORT_SKIP:-7021}
PORT_NOSKIP=${PORT_NOSKIP:-7022}
REQUESTS=${REQUESTS:-20}
REPEATS=${REPEATS:-3}

REPO=$(git rev-parse --show-toplevel)
cd "$REPO"
PATCH=$REPO/passage-no-offset-skip.patch
BUILD=$REPO/build
say() { printf '\n== %s\n' "$*"; }
die() { printf 'passage-offset-bench: %s\n' "$*" >&2; exit 1; }

# The link step emits dozens of harmless `ld: warning:` lines on macOS, so keep
# the build output in a log and only show it when something actually failed.
build() {  # $1... = cmake --build targets
  local log=$WORKDIR/build.log
  mkdir -p "$WORKDIR"
  if ! cmake --build "$BUILD" --target "$@" -j "$JOBS" >"$log" 2>&1; then
    printf 'the build failed, last 20 lines of %s:\n' "$log" >&2
    tail -20 "$log" >&2
    exit 1
  fi
}

# ---------------------------------------------------------------- preconditions
for t in awk curl bc cmake git python3; do
  command -v "$t" >/dev/null || die "$t is required but not on the PATH"
done
[ -f "$PATCH" ] || die "missing $PATCH"
[ -z "$(git status --porcelain -- src/)" ] ||
  die "src/ has uncommitted changes; commit or stash them first, because this
                            script patches and reverts src/index/CompressedRelationReader.cpp"
for p in "$PORT_SKIP" "$PORT_NOSKIP"; do
  ! (exec 3<>/dev/tcp/127.0.0.1/"$p") 2>/dev/null ||
    die "port $p is already in use (override with PORT_SKIP/PORT_NOSKIP)"
done

# `nproc` is GNU, `sysctl` is BSD, and neither is guaranteed; override with
# JOBS=n if the guess is wrong.
JOBS=${JOBS:-$( (command -v nproc >/dev/null 2>&1 && nproc) ||
                (command -v sysctl >/dev/null 2>&1 && sysctl -n hw.ncpu) ||
                echo 4 )}

# `date +%s.%N` is GNU; BSD date on older macOS prints a literal N, which would
# silently feed `bc` garbage. Fall back to python3 when that happens.
case $(date +%s.%N) in
  *N) now() { python3 -c 'import time; print(time.time())'; } ;;
  *)  now() { date +%s.%N; } ;;
esac

PATCH_APPLIED=0
SERVERS=()
cleanup() {
  local rc=$?
  for pid in ${SERVERS+"${SERVERS[@]}"}; do kill "$pid" 2>/dev/null || true; done
  if [ "$PATCH_APPLIED" = 1 ]; then
    say "reverting $PATCH"
    git apply -R "$PATCH" && PATCH_APPLIED=0
    git status --porcelain -- src/ | grep -q . &&
      printf 'WARNING: src/ is still modified, inspect it before committing\n' >&2
    say "rebuilding qlever-server from the restored source"
    cmake --build "$BUILD" --target qlever-server -j "$JOBS" \
      >"${WORKDIR:-/tmp}/build.log" 2>&1 ||
      printf 'WARNING: the final rebuild failed, see %s\n' \
        "${WORKDIR:-/tmp}/build.log" >&2
  fi
  exit $rc
}
trap cleanup EXIT INT TERM

start_server() {  # $1 = port, $2 = log
  "$BUILD/qlever-server" -i "$WORKDIR/testidx" -p "$1" -j 4 -m 4G >"$2" 2>&1 &
  SERVERS+=($!)
  local waited=0
  until curl -s -o /dev/null --max-time 2 "http://localhost:$1/"; do
    sleep 1; waited=$((waited + 1))
    [ "$waited" -lt 120 ] || die "server on port $1 did not come up, see $2"
  done
  # The very first batch a fresh server answers also pays for faulting the
  # index in, which would otherwise land entirely on whichever cell is measured
  # first and show up as a negative cost.
  bench "$1" 1 >/dev/null
}

ask() {  # $1 = port, $2 = offset, $3 = limit
  curl -s -X POST --data-urlencode \
    "query=SELECT * WHERE { ?s <http://ex.org/p1> ?o } LIMIT $3 OFFSET $2" \
    "http://localhost:$1/"
}

# `LIMIT`/`OFFSET` is part of the query cache key, so each request uses a
# different offset and every one of them is a cache miss.
bench() {  # $1 = port, $2 = base offset -> seconds for $REQUESTS requests
  local start i
  start=$(now)
  for ((i = 0; i < REQUESTS; i++)); do
    ask "$1" "$(($2 + i))" 5 >/dev/null
  done
  echo "$(now) - $start" | bc
}

median_of_batches() {  # $1 = port, $2 = base offset
  local r=() k
  bench "$1" "$(($2 + 900))" >/dev/null          # warm-up, discarded
  for ((k = 1; k <= REPEATS; k++)); do
    r+=("$(bench "$1" "$(($2 + k * 100))")")
  done
  printf '%s\n' "${r[@]}" | sort -n |
    awk '{a[NR]=$1} END {printf "%.3f", a[int((NR + 1) / 2)]}'
}

rows() {  # $1 = port, $2 = offset -- the bindings alone, for comparison
  ask "$1" "$2" 3 |
    python3 -c 'import json,sys; print(json.dumps(json.load(sys.stdin)["results"], sort_keys=True))'
}

# ----------------------------------------------------------------------- build
say "building qlever-index and qlever-server (-j $JOBS)"
build qlever-index qlever-server

# ------------------------------------------------------------------ data/index
mkdir -p "$WORKDIR"
if [ ! -s "$WORKDIR/data.ttl" ] ||
   [ "$(wc -l <"$WORKDIR/data.ttl")" -ne "$TRIPLES" ]; then
  say "generating $TRIPLES triples in $WORKDIR/data.ttl"
  awk -v n="$TRIPLES" 'BEGIN {
    for (i = 0; i < n; i++)
      printf "<http://ex.org/s%d> <http://ex.org/p1> <http://ex.org/o%d> .\n", i, i % 1000
  }' >"$WORKDIR/data.ttl"
fi
if [ ! -f "$WORKDIR/testidx.index.pso" ]; then
  say "building the index"
  ( cd "$WORKDIR" && "$BUILD/qlever-index" -i testidx -f data.ttl -F ttl -p true ) \
    2>&1 | grep -E 'Statistics for PSO|Index build completed' || true
fi
DEEP=$((TRIPLES - 100))

# -------------------------------------------------------- with the skipping
say "timing with the block skipping"
start_server "$PORT_SKIP" "$WORKDIR/server-skip.log"
SKIP_NEAR=$(median_of_batches "$PORT_SKIP" 0)
SKIP_DEEP=$(median_of_batches "$PORT_SKIP" "$DEEP")
ROWS_SKIP=$(rows "$PORT_SKIP" "$DEEP")

# ------------------------------------------------------- without the skipping
say "applying $PATCH and rebuilding qlever-server"
# The patch is context-based, so an upstream change to the surrounding lines
# can make it stop applying. Say what to do instead of leaving git's message.
git apply "$PATCH" || die "could not apply $PATCH.
                            src/index/CompressedRelationReader.cpp has probably
                            moved since the patch was made. Either regenerate
                            it, or add 'return 0;' as the first statement of
                            firstBlockAfterOffset by hand."
PATCH_APPLIED=1
build qlever-server

say "timing without the block skipping"
start_server "$PORT_NOSKIP" "$WORKDIR/server-noskip.log"
NOSKIP_NEAR=$(median_of_batches "$PORT_NOSKIP" 0)
NOSKIP_DEEP=$(median_of_batches "$PORT_NOSKIP" "$DEEP")
ROWS_NOSKIP=$(rows "$PORT_NOSKIP" "$DEEP")

# ---------------------------------------------------------------------- report
say "result   $TRIPLES triples, $REQUESTS requests per batch, median of $REPEATS"
printf '  %-28s OFFSET ~0   %s s   OFFSET ~%s   %s s\n' \
  'without the block skipping' "$NOSKIP_NEAR" "$DEEP" "$NOSKIP_DEEP"
printf '  %-28s OFFSET ~0   %s s   OFFSET ~%s   %s s\n' \
  'with it' "$SKIP_NEAR" "$DEEP" "$SKIP_DEEP"
printf '\n  cost of the offset, per query: %s ms without, %s ms with\n' \
  "$(echo "($NOSKIP_DEEP - $NOSKIP_NEAR) / $REQUESTS * 1000" | bc -l | xargs printf '%.1f')" \
  "$(echo "($SKIP_DEEP - $SKIP_NEAR) / $REQUESTS * 1000" | bc -l | xargs printf '%.1f')"

if [ "$ROWS_SKIP" = "$ROWS_NOSKIP" ]; then
  printf '  both configurations returned identical rows\n'
else
  printf '  ERROR: the two configurations returned DIFFERENT rows\n' >&2
  printf '    with:    %s\n    without: %s\n' "$ROWS_SKIP" "$ROWS_NOSKIP" >&2
  exit 1
fi
