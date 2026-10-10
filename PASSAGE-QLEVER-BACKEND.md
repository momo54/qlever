# A QLever backend for Passage

[Passage](https://github.com/passage-org/passage) answers a SPARQL query with
partial results plus a *continuation query* that returns the missing ones. Its
paper ([WWW '25](https://doi.org/10.1145/3696410.3714757)) states two
requirements on the storage underneath:

> (1) Triple pattern evaluations must return an ordered list of mappings […]
> (2) Each partial evaluation of a continuation query must ensure progress […]
> This requires efficient access to an offset in the triple patterns, a feature
> already supported in Blazegraph storage and HDT.

This directory adds a third storage that meets both, on top of
[QLever](https://github.com/ad-freiburg/qlever).

The motivation is maintenance, not speed. Passage's main backend is
Blazegraph, which is no longer maintained; QLever is actively developed. A
backend that merely reaches comparable performance is therefore already worth
having, because it replaces a dependency that has no future.

Requirement (1) holds for free: QLever enumerates a triple pattern in the order
of the permutation it is scanned in, which is deterministic and stable for a
fixed index and a fixed snapshot of the delta triples. Requirement (2) did not
hold — an `OFFSET` was applied by reading and discarding the triples it skips
over — and is the subject of the companion change described in
[PASSAGE-README.md](PASSAGE-README.md).

## Architecture

```
passage-volcano  (Java)          the continuation engine, unchanged
      │  Backend<Long,String> / BackendIterator<Long,String>
      ▼
QLeverBackend, QLeverIterator    src/libqlever/jni/java/
      │  QLeverNative, batches of 1024 matches
      ▼
libqleverjni  (JNI, C++)         src/libqlever/jni/QleverJni.cpp
      ▼
qlever::ScanCursor  (C++)        src/libqlever/ScanCursor.h|.cpp
      ▼
Permutation::lazyScan            QLever's index
```

`qlever::ScanCursor` is the piece that did not exist. `libqlever` exposed
QLever at the level of a whole query (`Qlever::query`), whereas an engine that
drives the joins itself needs a single scan that it can suspend, ask for its
offset, and resume elsewhere. The cursor is independent of Passage and usable
from C++ on its own.

### What the interface maps onto

| Passage | QLever |
| --- | --- |
| `Backend.search(s, p, o)` | `ScanCursor`, from a `ScanSpecification` |
| `Backend.searchDistinct(s, p, o, codes)` | a `ScanCursor` whose permutation makes `codes` lead |
| `Backend.any()` | an unbound component (`std::nullopt`) |
| `Backend.getId` / `getValue` / `getString` | the vocabulary |
| `PreemptIterator.skip(to)` | `lazyScan` with an `OFFSET`, which skips whole blocks |
| `PreemptIterator.current()` | the offset of the current match |
| `BackendIterator.cardinality()` | `Permutation::getResultSizeOfScan` |
| `BackendIterator.cardinality(strength)` | `Permutation::getSizeEstimateForScan` |
| `RandomIterator.random()` | `skip(random() * cardinality())`, as `HDTIterator` does |

The permutation is picked from the set of bound components by a fixed mapping
(`ScanCursor::permutationFor`), never by QLever's query planner, so the order
in which a pattern is enumerated depends on the pattern alone. This is what
makes an offset reported by one cursor meaningful to another.

All the scans of one `QLeverBackend` share a single
`LocatedTriplesSharedState`, so the offsets stay valid across the scans of a
whole sequence of continuation queries. An update applied in between would
shift them.

### When a scan is opened

A cursor reads nothing until it is first asked for a match. This is not an
implementation detail but a consequence of the `Backend` interface:
`search(s, p, o)` takes no offset, so `PassageScan` creates the scan and only
then calls `skip` to position it. A cursor that opened its scan eagerly would
decompress its first block just for the `skip` to discard it and open another
scan — two block decompressions per scan, on the path where a continuation
engine spends its time, since every scan it creates starts with an offset.
Deferring makes a `skip` issued before the first read free.

The visible consequence is that `hasNext`, `getId` and `getString` are not
`const`: they may open the scan.

### Distinct scans

`searchDistinct` is served by the same single scan, with no `Distinct`
operator, no sort and no hash table. The permutation is chosen so that the
components to deduplicate on come right after the bound ones, hence their
values are sorted and equal ones adjacent, and deduplicating is a matter of
advancing while they do not change.

That choice has to be made together with the bound components, not after them:
`?s <p> ?o` needs POS to enumerate distinct objects and PSO to enumerate
distinct subjects, same pattern and same bindings. For a triple pattern such an
order always exists, since there are only three components and all six of their
orders are materialized.

The offset of a distinct cursor counts distinct values, so a continuation for
such a leaf is `SELECT DISTINCT ?x WHERE { pattern } OFFSET k` — an ordinary
SPARQL query of constant size.

NOTE: `skip` on a distinct cursor walks the groups. The block skipping that
makes a large `OFFSET` cheap counts stored rows, and a distinct offset counts
groups, so it does not carry over. Making it logarithmic needs a count of
distinct values per block, which the block metadata does not have; it could be
computed at index build time in the pass that already computes `graphInfo_`.

### Crossing the boundary

`BackendIterator` is a tuple-at-a-time interface and QLever reads blocks of
about 31 000 rows, so one JNI call per match would dominate everything else.
`ScanCursor::fill` copies up to N matches at once, and `QLeverIterator` serves
`hasNext`/`next`/`getId` from a batch of 1024. The batch is invisible from the
outside: `current()` always reports the offset of the match that `getId`
returns, which is what a continuation query needs. `skip` inside the current
batch costs nothing at all.

## Building

### The native side

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target qleverjni qlever-index -j $(nproc)
```

The JNI bridge is only built when the JNI headers are found, so an ordinary
QLever build is unaffected. Set `JAVA_HOME` if they are somewhere unusual. The
result is `build/lib/libqleverjni.{so,dylib}`.

On macOS, follow `.github/workflows/macos-appleclang-native.yml`; see
[PASSAGE-README.md](PASSAGE-README.md#1-build) for the flags, and
[what breaks on macOS and why](PASSAGE-README.md#what-breaks-on-macos-and-why)
for the two failures that recipe does not cover — a Homebrew `abseil` shadowing
the pinned one, and `range-v3` against a recent Apple Clang.

### The Java side

`src/libqlever/jni/java/fr/gdd/passage/qlever/` holds three classes that depend
only on `passage-commons`. They belong in a `passage-qlever` module of the
Passage repository and live here next to the bridge they bind; move them there
as they are.

`passage-commons` is not published to any Maven repository, so it has to be
built from source:

```bash
git clone https://github.com/passage-org/passage.git
cd passage && mvn -pl passage-commons -am install -DskipTests
```

This installs `fr.gdd:passage-commons:0.2.0` locally. That version is the one
carried by the project's `master` rather than a release, so it moves; this
backend was written against commit `8d38952`.

```bash
javac -cp ~/.m2/repository/fr/gdd/passage-commons/0.2.0/passage-commons-0.2.0.jar:… \
      -d classes fr/gdd/passage/qlever/*.java
```

See [`src/libqlever/jni/java/README.md`](src/libqlever/jni/java/README.md) for
the full classpath, which `mvn dependency:build-classpath` produces.

## Running it

Build an index from the graph of Figure 2 of the paper:

```bash
cat > fig2.ttl <<'EOF'
<http://ex.org/H1> <http://ex.org/type> <http://ex.org/Human> .
<http://ex.org/H2> <http://ex.org/type> <http://ex.org/Human> .
<http://ex.org/H3> <http://ex.org/type> <http://ex.org/Human> .
<http://ex.org/H1> <http://ex.org/occupation> <http://ex.org/O1> .
<http://ex.org/H1> <http://ex.org/occupation> <http://ex.org/O2> .
<http://ex.org/H3> <http://ex.org/occupation> <http://ex.org/O3> .
EOF
build/qlever-index -i fig2 -f fig2.ttl -F ttl
```

Then drive `passage-volcano` over it, one scan per execution so that every step
yields a continuation query:

```java
QLeverBackend backend = new QLeverBackend(pathToLibqleverjni, "fig2");
String current = """
        SELECT * WHERE {
          ?human <http://ex.org/type> <http://ex.org/Human> .
          ?human <http://ex.org/occupation> ?occupation
        }""";
while (Objects.nonNull(current)) {
    PassageExecutionContext<Long, String> context =
        new PassageExecutionContextBuilder<Long, String>()
            .setName("PUSH").setMaxScans(1L).setMaxParallel(1)
            .setBackend(backend)
            .setExecutorFactory(ec ->
                new PassagePushExecutor<>((PassageExecutionContext<?, ?>) ec))
            .build();
    Op paused = context.executor.execute(
        Algebra.compile(QueryFactory.create(current)), results::add);
    current = Objects.nonNull(paused)
        ? OpAsQuery.asQuery(new Quad2Pattern().visit(paused)).toString()
        : null;
}
```

The evaluation terminates after 6 executions with the 3 expected solutions, and
the continuation queries have the shape of Example 3.10 of the paper. Step 2,
verbatim:

```sparql
SELECT * WHERE {
    { BIND(<http://ex.org/H3> AS ?human)
      ?human <http://ex.org/occupation> ?occupation }
  UNION
    { { SELECT * WHERE { ?human <http://ex.org/type> <http://ex.org/Human> }
        OFFSET 2 }
      ?human <http://ex.org/occupation> ?occupation } }
```

The commands that run this — the native library, the classpath, which JDK, and
the index — are in
[`src/libqlever/jni/java/README.md`](src/libqlever/jni/java/README.md), as
`PassageOnQlever`. Rerun on 2026-10-10 against the tree rebased onto upstream,
it still terminates after 6 executions with those 3 solutions and prints the
query above verbatim. Note that the process then exits 132 rather than 0,
for a reason unrelated to the evaluation that the same README documents.

## A convention that is easy to get wrong

`PassageScan` signals an unbound component of a triple pattern with **`null`**,
not with `Backend.any()`: it hands the output of `Substitutor` straight to
`Backend.search`. A backend therefore has to map `null` itself, as `HDTBackend`
does:

```java
return new QLeverIterator(backend, Objects.isNull(s) ? any() : s, …);
```

This is not stated on the `Backend` interface, and it is invisible to any test
that builds its scans itself — those pass `any()` explicitly. The first run of
a real query through `passage-volcano` is what surfaces it.

A second one: on a concrete `Backend<Long, String>`, the calls
`getId(String, int...)` and `getId(VALUE, int...)` are ambiguous and `javac`
refuses them. Passage itself only ever calls `getId` from generic code, where
`VALUE` is a type variable, so the ambiguity does not arise there.

## What is verified, and what is not

Verified:

* `ScanCursorTest` (6 tests) covers the enumeration, `skip`, the choice of
  permutation, and the property the whole design rests on — that the offset
  reported by one cursor, applied to a freshly created one, restores exactly
  the same position.
* Through the JNI bridge on a 2,000,000 triple index, `skip` to offset
  1,999,990 and the matching solution come back in well under a millisecond.
  Against the same build without the block skipping, 200 random deep skips cost
  6.125 ms each instead of 0.430 ms.
* 200 iterations of `search` + `skip` to a random offset + read, which is the
  pattern a continuation engine repeats: 0.416 ms each, against 0.764 ms for a
  cursor that opens its scan eagerly.
* A BGP through `passage-volcano`, as above: termination, and complete and
  correct results.

The programs behind these numbers are in
[`src/libqlever/jni/java/demo`](src/libqlever/jni/java/demo), with a
[README](src/libqlever/jni/java/README.md) saying which index each expects.

## Against Blazegraph, on WatDiv 10M

Measured on 10,916,457 triples, the same data on both sides, with the two
stores driving the same engine, by `QLeverVsBlazegraphTest` in the Passage
fork. First 200,000 solutions, median of five timed runs after a warm-up:

| | QLever /s | Blazegraph /s | |
|---|---|---|---|
| `?s ?p ?o` | **5,000,000** | 3,846,153 | QLever 1.3x |
| `?s a ?c` | **8,012,647** | 4,540,500 | QLever 1.8x |
| `?s a ?c . ?s ?p ?o` | 1,398,601 | **2,985,074** | Blazegraph 2.1x |

They answer identically: the join restricted to `wsdbm:Genre8`, the least
populated class, has eight solutions, the same on both. QLever's index is
283 MB against 1.7 GB.

So QLever reads faster and Blazegraph joins faster. The join is a sequence of
point lookups, which is what a B-tree is for and what a block-compressed column
store is not — but the factor was ninety-five before a cache of decompressed
blocks, and is two after it.

That cache is the finding here. Opening a scan decompresses a block and nothing
keeps it, so a nested-loop join, which opens one per outer solution and lands
in the same block thousands of times running, decompresses that block
thousands of times. Asking for the *same* subject two thousand times costs
0.919 ms each. It is not the index size (0.853 ms over a million triples,
0.861 over eleven million), not the prefetch depth (0.856 to 0.860 ms as
`lazy-index-scan-num-blocks-in-flight` goes from 20 to 1), and not the JNI
boundary (0.0004 ms to construct a cursor in C++, 0.933 for its first read).

None of which is an oversight: a merge join never returns to a block, so for
QLever's own engine a cache would be pure overhead, and large blocks are the
right trade for the sequential throughput it wants.
`lazy-index-scan-block-cache-size` is therefore zero by default and
`QLeverBackend` sets it to 16, taking the join from 24,384 solutions/s to
1,418,439 — fifty-eight times.

Two methodological notes, because both cost a wrong conclusion before being
noticed.

"Warm" is two things and treating them as one hid a bad protocol. The JVM
compiles the pipeline as it runs, and a store's working set stays in memory
once touched, so after a few runs both are joining in memory. The run-by-run
rates show the climb: Blazegraph goes 471,698 then 2,222,222 then 2,469,135
before levelling at 2,816,901, QLever 405,679 then 1,226,993 before levelling
at 1,418,439. Blazegraph needs four runs where QLever needs two, so discarding
one under-reports it, and whichever store runs second starts on an
already-compiled JVM — measured first Blazegraph opens at 268,456 and QLever at
405,679. The plateau does not depend on the order; the first run entirely does.
Three runs are discarded now, and an earlier version of this section, which
discarded none, claimed QLever ahead on the join on that basis.

And smaller blocks are worth sixteenfold *without* the block cache and
counterproductive with it, since one decompressed block then serves thousands
of lookups — so `build-index.sh` uses QLever's default size.

A third observation, unexplained: over twelve runs of the join the process
grows by about 840 MB with QLever and about 206 MB with Blazegraph, so the
disk ratio reverses in memory. QLever's resident footprint exceeds its entire
283 MB index, which its vocabulary and the structures it expands at load would
account for, but that is a guess and not a measurement.

Against the bar this backend was built to clear — replacing an unmaintained
dependency at comparable cost — a factor of two on joins against a sixfold
smaller index, and an advantage on scans, is cleared.

## A convention that is easy to get wrong

`PassageScan` signals an unbound component of a triple pattern with **`null`**,
not with `Backend.any()`: it hands the output of `Substitutor` straight to
`Backend.search`. A backend therefore has to map `null` itself, as `HDTBackend`
does:

```java
return new QLeverIterator(backend, Objects.isNull(s) ? any() : s, …);
```

This is not stated on the `Backend` interface, and it is invisible to any test
that builds its scans itself — those pass `any()` explicitly. The first run of
a real query through `passage-volcano` is what surfaces it.

A second one: on a concrete `Backend<Long, String>`, the calls
`getId(String, int...)` and `getId(VALUE, int...)` are ambiguous and `javac`
refuses them. Passage itself only ever calls `getId` from generic code, where
`VALUE` is a type variable, so the ambiguity does not arise there.

## What is verified, and what is not

Verified:

* `ScanCursorTest` (6 tests) covers the enumeration, `skip`, the choice of
  permutation, and the property the whole design rests on — that the offset
  reported by one cursor, applied to a freshly created one, restores exactly
  the same position.
* Through the JNI bridge on a 2,000,000 triple index, `skip` to offset
  1,999,990 and the matching solution come back in well under a millisecond.
  Against the same build without the block skipping, 200 random deep skips cost
  6.125 ms each instead of 0.430 ms.
* 200 iterations of `search` + `skip` to a random offset + read, which is the
  pattern a continuation engine repeats: 0.416 ms each, against 0.764 ms for a
  cursor that opens its scan eagerly.
* A BGP through `passage-volcano`, as above: termination, and complete and
  correct results.

The programs behind these numbers are in
[`src/libqlever/jni/java/demo`](src/libqlever/jni/java/demo), with a
[README](src/libqlever/jni/java/README.md) saying which index each expects.

## Against Blazegraph, and the block size that decides it

Measured on WatDiv 10M — 10,916,457 triples, the same data on both sides — with
the two stores driving the same engine, by `QLeverVsBlazegraphTest` in the
Passage fork. They answer identically; what differs is what they cost, and the
answer turned on a setting rather than on either design.

The query is `SELECT * WHERE { ?s a ?c . ?s ?p ?o }`, a nested-loop join, so
its inner pattern is scanned afresh for every outer solution. With QLever's
default index that is ruinous and with a smaller block size it is not:

| rows per block | opening a scan | the join | `?s ?p ?o` | index |
|---|---|---|---|---|
| 31,250 (QLever's default) | 0.915 ms | 24,000 /s | 628,930 /s | 283 MB |
| 1,000 | **0.048 ms** | 385,356 /s | 651,465 /s | 297 MB |
| 100 | 0.016 ms | 613,496 /s | 576,368 /s | 610 MB |
| Blazegraph | 0.072 ms | 743,494 /s | 692,041 /s | 1.7 GB |

The cost is decompressing the first block, which a block-compressed column
store pays whole however few rows are wanted. It is not the index size (0.853 ms
over a million triples, 0.861 over eleven million), not the prefetch depth
(0.856 to 0.860 ms as `lazy-index-scan-num-blocks-in-flight` goes from 20 to 1),
and not the JNI boundary (0.0004 ms to construct a cursor in C++, 0.933 for its
first read). A block holds 250 kB per column, so one triple can cost a megabyte
of Zstd, which is the millisecond measured.

So the default is not a bad one: it is the right trade for merge joins, which
is what QLever's engine does, and large blocks give those the sequential
throughput and the compression ratio they want. Passage cannot merge-join,
because a continuation has to stop mid-join and resume from a query, so it
nests loops and reads a handful of rows per scan. The same storage, the
opposite access pattern.

At 1,000 rows per block the join is sixteen times faster for five percent more
disk, long scans are unchanged, and opening a scan already costs less than
Blazegraph does. `build-index.sh` in the Passage module builds indexes that
way; the figures above are what it is for.

That setting, though, is a workaround for something else. The same 2,000 scans
cost 0.923 ms each when their subjects come out of the index in order and
0.922 ms when shuffled — no difference at all, although at 31,250 rows a block
spans some 400 consecutive subjects. Four hundred lookups into one block
decompress that same megabyte four hundred times, because QLever caches query
results and vocabulary lookups but not decompressed blocks.

Which is the same design choice again, not an oversight: a merge join never
returns to a block, so keeping one would be dead weight.

For a nested-loop join it is the whole cost, and the headroom is two orders of
magnitude rather than one. Asking for the *same* subject two thousand times
still costs 0.928 ms each, so that one block is decompressed two thousand
times; reading two thousand rows through a single scan costs 0.0020 ms each.

| 2,000 lookups, default blocks | ms each |
|---|---|
| subjects in index order | 0.948 |
| subjects shuffled | 0.925 |
| the same subject every time | 0.928 |
| 2,000 rows through one scan | **0.0020** |

So a cache of a few decompressed blocks is worth about 460x on this access
pattern, against the 19x the block size buys. A few entries suffice, one per
scan position that is live at once, which is the join depth times the
parallelism; a block is 250 kB per column, so the whole cache is a few
megabytes. It belongs in `CompressedRelationReader`, where this fork already
patches the `OFFSET` path.

Three things make it a change to design rather than to write. It has to be
keyed on the delta-triples version as well as the block, or an update would be
invisible to it — `LocatedTriplesState` already carries a counter for that. It
has to not take a lock on a path that merge joins walk constantly, since for
them every lookup is a miss. And for the same reason it has to be off by
default, which makes its natural shape a runtime parameter that an engine with
a different access pattern turns on, rather than a new behaviour imposed on
QLever's own.

What is left runs both ways rather than one. On the join QLever stays 1.9x
behind at 1,000 rows and 1.3x at 100; on a scan with the predicate bound it is
twice as fast; and it holds the same data in 297 MB against 1.7 GB. Against the
bar this backend was built to clear — replacing an unmaintained dependency at
comparable cost — that is cleared, provided the index is built for it.

Not verified, in decreasing order of how much it matters:

* **Scale.** The Passage run uses six triples; it exercises correctness, not
  the offset skipping.
* **Coverage.** Only basic graph patterns were driven end to end. `OPTIONAL`,
  `FILTER` and `UNION` go through the same `Backend` interface and should
  follow, but they were not run.
* **Skipping a distinct scan.** Correct, but it walks the groups rather than
  skipping blocks, so a large `OFFSET` on a high-cardinality distinct column is
  linear in the offset. See the note above.
* **Quads.** `search(s, p, o, c)` throws `UnsupportedOperationException`.
  QLever stores quads natively and the cursor does not expose the graph column
  yet; adding it is straightforward.
* **Concurrency.** One `ScanCursor` per thread; the cursor is not thread-safe,
  and `setMaxParallel` above 1 was not tried.
