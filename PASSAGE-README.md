# Skipping blocks for a large `OFFSET` on a triple pattern

This patch makes `OFFSET` on a single triple pattern cost constant time instead
of time proportional to the offset.

It was written for [Passage](https://github.com/passage-org/passage), whose
continuation queries carry their state as an `OFFSET` on a triple pattern; see
[PASSAGE-QLEVER-BACKEND.md](PASSAGE-QLEVER-BACKEND.md) for the backend that
builds on it. The change itself is independent of that use, and speeds up any
`OFFSET` pushed down to a single triple pattern.

## The problem

`IndexScan` already pushes `LIMIT`/`OFFSET` all the way down into the
permutation scan: `IndexScan::handlesLimitOffset()` returns `FULL`, and
`IndexScan::computeResult` forwards `getLimitOffset()` to
`Permutation::scan` / `Permutation::lazyScan`.

However, the `OFFSET` was only ever consumed by `pruneBlock`, which operates on
a block that has already been **read from disk, decompressed and
post-processed**. Skipping *n* rows therefore read and decompressed
`n / blocksize` blocks just to throw their rows away, making `OFFSET n` cost
O(n).

The information needed to avoid this is already in memory. The block metadata
of a permutation (`IndexMetaData::blockData_`) is fully resident in RAM and
carries `numRows_` for every block; it was simply never consulted when applying
an `OFFSET`.

## The change

Two additions, no existing line modified.

`CompressedRelationReader::FilterDuplicatesAndGraphs::numRowsIsExact`
(`src/index/CompressedRelationReader.h`,
`src/index/CompressedRelationReader.cpp`) reports whether `postprocessBlock`
will leave a block's row count untouched:

```cpp
return graphFilter_.areAllGraphsAllowed() &&
       !blockMetadata.containsDuplicatesWithDifferentGraphs_;
```

`firstBlockAfterOffset` walks that same metadata in RAM and returns the index
of the first block that the `OFFSET` does not consume entirely, subtracting the
rows of the blocks before it from the offset.
`CompressedRelationReader::lazyScan` calls it once and has its generator start
at that block, so the skipped ones are never read:

```cpp
size_t firstBlock = 0;
while (firstBlock + 1 < blocks.size() &&
       offset >= blocks[firstBlock].numRows_ &&
       blockIsSkippable(blocks[firstBlock])) {
  offset -= blocks[firstBlock].numRows_;
  ++firstBlock;
}
```

The rest of the scan is untouched: `pruneBlock` then applies the remaining
offset (always smaller than one block) to the first block that is actually
read, and `LIMIT` handling, delta-triple merging and graph post-processing are
unchanged.

### When a block may be skipped

`numRows_` counts *physical rows*. It equals the number of rows a block
contributes to the result only under all three of these conditions, which
`blockIsSkippable` checks:

1. `numRowsIsExact(block)` — no row is dropped by the graph filter, and the
   block contains no duplicates that differ only in their graph. Note that
   QLever's default graph is the union of all graphs
   (`QueryPlanner::getActiveGraphs` returns `GraphFilter::All()` for a pattern
   without `FROM`/`GRAPH`), so such duplicates must be deduplicated to restore
   RDF set semantics, and a block containing them contributes fewer rows than
   it stores.
2. `!locatedTriplesPerBlock_.containsTriples(block.blockIndex_)` — the block is
   not modified by delta triples. `LocatedTriplesPerBlock::numTriples` is only
   an *upper* bound (a DELETE may target a triple that does not exist), so
   updated blocks cannot be corrected for and are excluded rather than
   estimated.
3. `isTripleInSpecification(scanSpec_, block.firstTriple_)` and the same for
   `lastTriple_` — the whole block belongs to the result, so
   `readPossiblyIncompleteBlock` would not drop any row either.

If any condition fails for a block, skipping stops there and the pre-existing
code path consumes the rest of the offset. The result is then exactly as
before, just as slow as before. The last block of the range is never skipped,
which preserves the `beginBlockMetadata_ < endBlockMetadata_` invariant of
`get()` even for an `OFFSET` larger than the result.

The offset is counted on the post-filter, post-deduplication sequence, so
SPARQL semantics are preserved: `OFFSET n` skips *n* solutions, not *n* stored
rows.

### Scope

All four shapes of a triple pattern benefit. `ScanSpecification::validate`
enforces that the fixed columns are always a prefix of `(col0, col1, col2)`, so
there are only four:

| Pattern | Fixed | Behaviour |
| --- | --- | --- |
| `?s ?p ?o` | nothing | every block skippable |
| `?x <p> ?o` | `col0` | all blocks of a large relation skippable; a small relation occupies a single block anyway |
| `<s> <p> ?o` | `col0`, `col1` | interior blocks skippable, up to two boundary blocks read |
| `<s> <p> <o>` | all | at most one block |

The limiting factor is the surrounding query, not the pattern. `IndexScan` must
be the root of the query execution tree for `LIMIT`/`OFFSET` to reach it at
all. `Join`, `Filter`, `Distinct`, `OrderBy` and `GroupBy` do not override
`handlesLimitOffset()` and therefore keep the clause above themselves, which is
correct — a join may eliminate arbitrary rows, so the offset cannot be pushed
through it.

`OFFSET` and prefiltering remain mutually exclusive, as they were before:
`IndexScan::getUpdatedQueryExecutionTreeWithPrefilterApplied` bails out when
`LIMIT`/`OFFSET` constrains the scan.

## Reproducing from the command line

The measurement below builds a real index and compares the same query against a
server built with and without the patch.

### 1. Build

Two commands, and both are needed. The first only *configures*: it generates
the build files and compiles nothing. The binaries appear during the second.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target qlever-index qlever-server CompressedRelationsTest -j $(nproc)

QLEVER=$PWD/build   # the steps below change directory, so name it now
```

`cmake --build` writes `build/qlever-index` and `build/qlever-server`, which is
where step 3 and step 4 below expect them.

On macOS, replace the configure command with the one below. It is
`.github/workflows/macos-appleclang-native.yml`, the authoritative recipe, plus
the `range-v3` flag that a recent Apple Clang needs — see
[what breaks on macOS and why](#what-breaks-on-macos-and-why), which also
covers the unrelated `abseil` trap. The build command is the same one as above,
except that `nproc` is GNU coreutils and is not part of a stock macOS:

```bash
brew install boost icu4c openssl@3 zstd jemalloc pkg-config

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
      -DLOGLEVEL=INFO \
      -DUSE_PARALLEL=false \
      -D_NO_TIMING_TESTS=ON \
      -DCOMPILER_SUPPORTS_MARCH_NATIVE=FALSE \
      -DICU_ROOT=$(brew --prefix icu4c) \
      -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3) \
      -DCMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE=PRE_TEST \
      -DCMAKE_CXX_FLAGS=-DMETA_NO_STD_FORWARD_DECLARATIONS

cmake --build build --target qlever-index qlever-server CompressedRelationsTest \
      -j $(sysctl -n hw.ncpu)

QLEVER=$PWD/build   # the steps below change directory, so name it now
```

#### What breaks on macOS and why

That recipe is necessary but not sufficient on a developer machine, because the
CI runner has nothing installed besides the six formulae above. Both failures
below were hit while rebasing this branch onto upstream in October 2026, with
Apple Clang 21 and Homebrew.

**A Homebrew `abseil` shadows the one QLever pins.** On macOS,
`CMakeLists.txt` puts the Homebrew prefix on the *global* include path:

```cmake
link_directories(${HOMEBREW_PREFIX}/lib)
include_directories(${HOMEBREW_PREFIX}/include)
```

QLever fetches abseil itself, pinned to `20260107.1`. A Homebrew `abseil` is
newer (`20260817.0` at the time of writing), and because that
`include_directories` comes first, its headers are the ones that get included
while the pinned library is the one that gets linked. Unlinking it is enough,
and is reversible:

```bash
brew unlink abseil        # `brew link abseil` restores it
```

It fails loudly rather than silently: each abseil LTS release puts its whole
API in a versioned inline namespace, so the Homebrew headers emit
`absl::lts_20260817::...` symbols while the pinned library only defines
`absl::lts_20260107::...`, and the link step ends with

```
Undefined symbols for architecture arm64:
  "absl::lts_20260817::StrCat(absl::lts_20260817::AlphaNum const&, ...)",
      referenced from: ...
ld: symbol(s) not found for architecture arm64
```

Any undefined `absl::lts_<date>` symbol whose date does not match the comment
next to `abseil`'s `GIT_TAG` in `CMakeLists.txt` is this and nothing else.

The macOS CI never installs abseil, which is why this is invisible there. The
same reasoning applies to any dependency that QLever pins *and* Homebrew
provides, so a build that fails in a fetched dependency is worth checking
against `brew list` before anything else.

**`range-v3` does not compile against a very recent libc++.** QLever fetches
`range-v3` from the `joka921/range-v3` fork; with Apple Clang 21, its
`meta.hpp` fails with `redefinition of 'allocator' as a different kind of
symbol`. `range-v3` ships a guard for exactly this case:

```bash
-DCMAKE_CXX_FLAGS=-DMETA_NO_STD_FORWARD_DECLARATIONS
```

NOTE: a `cmake` configure that succeeds says nothing about either problem. Both
are compile-time failures in a dependency's headers, so they only appear once a
translation unit that includes them is actually compiled.

One thing that looks like an error and is not: the link step prints a long
series of `ld: warning: building for macOS-11.0, but linking with dylib ...
built for newer version`, because `CMAKE_OSX_DEPLOYMENT_TARGET=11.0` is much
older than the target of the current Homebrew dylibs. Those are warnings only.

One that was a real error, and is now fixed upstream: between January and
October 2026, the `re2` `FetchContent_Declare` combined `GIT_SHALLOW TRUE` with
a commit that was no longer a branch tip, so configuring failed while checking
`re2` out. Upstream `43b1b086` (PR #3603) replaced the shallow clone with a
full one, so an older checkout of this fork only needs rebasing onto upstream.

### 2. Create the data

Twenty million triples sharing a single predicate, so that one relation spans
many blocks. With the default block size of 250 KB per column a block holds
31 250 rows, giving 640 blocks. That number is what the measurement is about:
at 64 blocks the effect is nearly hidden by the concurrency of the scan itself,
as the second row of the table in step 6 shows.

```bash
mkdir -p /tmp/qlever-demo && cd /tmp/qlever-demo
awk 'BEGIN{for(i=0;i<20000000;i++) printf "<http://ex.org/s%d> <http://ex.org/p1> <http://ex.org/o%d> .\n", i, i%1000}' > data.ttl
```

This writes 1.3 GB and takes about 13 s; the index below adds 1.8 GB.

### 3. Build the index

```bash
$QLEVER/qlever-index -i testidx -f data.ttl -F ttl -p true
```

About 20 s. The log confirms the geometry the measurement relies on:

```
Statistics for PSO: #relations = 1, #blocks = 640, #triples = 20,000,000
```

### 4. Start the server

```bash
$QLEVER/qlever-server -i testidx -p 7021 -j 4 -m 4G
```

### 5. Run queries

```bash
curl -s -X POST --data-urlencode \
  "query=SELECT * WHERE { ?s <http://ex.org/p1> ?o } LIMIT 3 OFFSET 19999990" \
  http://localhost:7021/
```

The rows come back as `s9999990`, `s9999991`, `s9999992` rather than
`s19999990`: a scan walks the permutation in `Id` order, which is the
collation order of the vocabulary strings, and `s9999990` sorts near the end
among `s1`, `s10`, `s100`, ... That is the deterministic-but-arbitrary order
this patch preserves, not a defect.

### 6. Measure

Steps 1 to 6 are also a script, which is the only way to run the comparison
without editing C++ by hand:

```bash
./passage-offset-bench.sh              # 20 M triples
./passage-offset-bench.sh 2000000      # the smaller size
```

It builds both binaries, generates the data and the index when they are not
already there, times the queries with the skipping, then applies
`passage-no-offset-skip.patch`, rebuilds `qlever-server`, times them again, and
reverts. The patch is reverted and the binary rebuilt on every exit path,
Ctrl-C included; the script refuses to start while `src/` has uncommitted
changes, so a failed revert cannot be mistaken for your own work; and it
compares the rows returned by the two configurations, failing if they differ.
Run on 2026-10-10 it reproduced the 20 M figure below at 32.4 ms per query
against 32.5 ms.

The rest of this section is what the script does, for when you would rather do
it by hand.

`LIMIT`/`OFFSET` is part of the cache key, so vary the offset across iterations
to keep every request a cache miss. Compare a batch near the start of the
relation with a batch near its end.

```bash
bench() {  # $1 = port, $2 = base offset
  local start=$(date +%s.%N)
  for i in $(seq 0 19); do
    curl -s -o /dev/null -X POST --data-urlencode \
      "query=SELECT * WHERE { ?s <http://ex.org/p1> ?o } LIMIT 5 OFFSET $(($2+i))" \
      "http://localhost:$1/"
  done
  echo "$(date +%s.%N) - $start" | bc
}

bench 7021 0           # near the start of the relation
bench 7021 19999900    # near its end
```

To measure the behaviour without the skipping on the same binary, the same
index and the same server flags, make `firstBlockAfterOffset` return `0` before
it inspects anything, and run a second server from that build. Comparing two
separate builds instead invites differences that have nothing to do with the
`OFFSET`.

Median of three batches of 20 requests, each preceded by a discarded warm-up
batch; Apple M-series, 16 GB, measured on 2026-10-10 against the tree rebased
onto upstream:

```
20 M triples, 640 blocks
  without the block skipping   OFFSET ~0   0.320 s   OFFSET ~19.9999M   0.970 s   (x3.0)
  with it                      OFFSET ~0   0.327 s   OFFSET ~19.9999M   0.331 s   (flat)

2 M triples, 64 blocks
  without the block skipping   OFFSET ~0   0.320 s   OFFSET  ~1.9999M   0.382 s   (+19 %)
  with it                      OFFSET ~0   0.322 s   OFFSET  ~1.9999M   0.297 s   (flat)
```

Both configurations were checked to return identical rows, which is worth
asserting next to the timing rather than assuming.

The two sizes together are the actual result, because they show the cost is
linear in the number of blocks the `OFFSET` passes over:

| triples | blocks | cost per query | per block |
|---|---|---|---|
| 2 M | 64 | 3.1 ms | 0.048 ms |
| 20 M | 640 | 32.5 ms | 0.051 ms |

Ten times the blocks, ten times the cost, and the per-block figure is stable —
that is the `O(offset)` the patch removes, and with the skipping both sizes are
flat instead.

At 64 blocks the effect is easy to miss, because `lazyScan` reads up to 21
blocks concurrently (`lazy-index-scan-num-blocks-in-flight`, default 20): 64
discarded blocks cost about three rounds, which is nearly lost in the ~16 ms a
request spends elsewhere. That parameter bounds how many blocks are in flight,
not how many are read, so the masking does not survive scale: at 640 blocks the
same query is three times slower end to end.

In the `with it` rows a deep offset sometimes measures a hair *faster* than
`OFFSET ~0`, so the script can print a small negative cost there. The likely
reason, which this measurement does not isolate, is that at `OFFSET ~0` the
scan reads ahead into blocks the `LIMIT` then discards, whereas with the
skipping a deep offset starts at the last block and has nothing left to read
ahead into.

NOTE: an earlier version of this section reported +53 % and 9.4 ms per query at
2 M triples. Those figures were taken before this branch was rebased onto
upstream, from two separate builds, and they do not reproduce.

## Tests

`test/CompressedRelationsTest.cpp` gains two tests. Both use a helper that
returns the scan result together with `LazyScanMetadata::numBlocksRead_`, so
the skipping is asserted on a counter rather than on a timing, which keeps the
tests deterministic.

`largeOffsetSkipsWholeBlocks` covers the two opposite situations. With all
triples in one graph, offsets from 0 to beyond the end of the relation must
return exactly the corresponding suffix of the complete scan while reading
fewer than half the blocks. With every triple present in two graphs, all blocks
carry `containsDuplicatesWithDifferentGraphs_`, and the test asserts that
`numBlocksRead_` is **identical** to the unconstrained scan: nothing may be
skipped, and the result must still be correct. That second assertion is the
important one, since the most plausible regression is someone relaxing one of
the three guards believing it redundant.

`largeOffsetSkipsBlocksForAllScanSpecifications` repeats the check for a scan
with no fixed column and for one with `col0` and `col1` fixed.

```bash
./build/test/CompressedRelationsTest --gtest_filter='*largeOffset*'
```

NOTE: run the test binaries from a scratch directory rather than from the root
of the checkout. They write index files named after the test into the current
working directory, and `.gitignore` only covers `/_staticGlobalTestIndex*`, so
running the whole suite in the checkout leaves a couple of hundred untracked
files behind.

Both tests fail against the unpatched code, and fail only on the block-skipping
assertions — the correctness assertions pass in both versions, as they must,
since the original code is correct and merely slow.

Related suites that exercise the same scan path: `IndexScanTest`, `IndexTest`,
`DeltaTriplesTest`, `LocatedTriplesTest`, `PermutationTest`.

## Possible follow-ups

Deliberately left out to keep this change reviewable:

- `getScanConfig` always adds the graph column to the columns read, even when
  it is only deleted again afterwards. Under the same conditions that
  `numRowsIsExact` checks, that column is unnecessary, which would cut the
  bytes read for `?x <p> ?o` by a third.
- `OFFSET` and prefiltering could be combined instead of being mutually
  exclusive.
- Random access by rank is currently a linear walk over the block metadata of
  the relation. That walk is not a bottleneck, because
  `Permutation::lazyScanImpl` already materialises a `std::vector` of the
  relevant blocks and thus pays O(number of blocks) unconditionally. Should
  that materialisation ever disappear, a prefix-sum array over `numRows_` would
  turn the seek into a binary search.
