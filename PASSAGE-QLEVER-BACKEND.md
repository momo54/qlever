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

Not verified, in decreasing order of how much it matters:

* **Throughput.** Nothing here measures how this backend compares to the
  Blazegraph one on a real workload (WDBench), nor what the JNI boundary
  actually costs. Note what the bar is: the point of this backend is to replace
  an unmaintained dependency, so comparable performance suffices and only a
  large regression would be a problem.
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
