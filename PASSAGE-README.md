# Skipping blocks for a large `OFFSET` on a triple pattern

This patch makes `OFFSET` on a single triple pattern cost constant time instead
of time proportional to the offset.

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

`firstBlockAfterOffset` walks that same metadata in RAM and returns the index of
the first block that the `OFFSET` does not consume entirely, subtracting the
rows of the blocks before it from the offset. `CompressedRelationReader::lazyScan`
calls it once and has its generator start at that block, so the skipped ones are
never read:

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

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target qlever-index qlever-server CompressedRelationsTest -j $(nproc)
```

On macOS, follow `.github/workflows/macos-appleclang-native.yml`, which is the
authoritative recipe:

```bash
brew install boost icu4c openssl@3 zstd jemalloc pkg-config
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
      -DUSE_PARALLEL=false \
      -DCOMPILER_SUPPORTS_MARCH_NATIVE=FALSE \
      -DICU_ROOT=$(brew --prefix icu4c) \
      -DCMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE=PRE_TEST \
      -D_NO_TIMING_TESTS=ON
```

With a very recent Apple Clang, the vendored `range-v3` fails to compile
against libc++ (`meta.hpp`: `redefinition of 'allocator' as different kind of
symbol`). Adding `-DCMAKE_CXX_FLAGS=-DMETA_NO_STD_FORWARD_DECLARATIONS` works
around it; `range-v3` provides that guard for exactly this case.

### 2. Create the data

Two million triples sharing a single predicate, so that one relation spans many
blocks. With the default block size of 250 KB per column, a block holds 31 250
rows, giving 64 blocks.

```bash
mkdir -p /tmp/qlever-demo && cd /tmp/qlever-demo
awk 'BEGIN{for(i=0;i<2000000;i++) printf "<http://ex.org/s%d> <http://ex.org/p1> <http://ex.org/o%d> .\n", i, i%1000}' > data.ttl
```

### 3. Build the index

```bash
/path/to/build/qlever-index -i testidx -f data.ttl -F ttl -p true
```

The log confirms the geometry the measurement relies on:

```
Statistics for PSO: #relations = 1, #blocks = 64, #triples = 2,000,000
```

### 4. Start the server

```bash
/path/to/build/qlever-server -i testidx -p 7021 -j 4 -m 4G
```

### 5. Run queries

```bash
curl -s -X POST --data-urlencode \
  "query=SELECT * WHERE { ?s <http://ex.org/p1> ?o } LIMIT 3 OFFSET 1999990" \
  http://localhost:7021/
```

### 6. Measure

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

bench 7021 0          # near the start of the relation
bench 7021 1999900    # near its end
```

Measured on 2 M triples, 20 requests per batch:

```
without the patch    OFFSET ~0   0.355 s     OFFSET ~1.999M   0.542 s   (+53 %)
with the patch       OFFSET ~0   0.324 s     OFFSET ~1.999M   0.301 s   (flat)
```

Without the patch the offset costs about 9.4 ms per query — the cost of
decompressing the 64 blocks that are then discarded. With the patch that cost
disappears into the noise. Both versions return identical results.

The absolute gain is small here because 64 blocks is not much. What the
measurement establishes is the shape of the curve: linear in the offset before,
flat after. On a relation of 100 M triples (about 3 200 blocks) the same
mechanism removes two orders of magnitude of work instead of one.

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
