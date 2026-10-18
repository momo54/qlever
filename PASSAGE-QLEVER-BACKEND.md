# The QLever side of a Passage backend

[Passage](https://github.com/passage-org/passage) answers a SPARQL query with
partial results plus a *continuation query* that returns the missing ones. Its
paper ([WWW '25](https://doi.org/10.1145/3696410.3714757)) states two
requirements on the storage underneath:

> (1) Triple pattern evaluations must return an ordered list of mappings […]
> (2) Each partial evaluation of a continuation query must ensure progress […]
> This requires efficient access to an offset in the triple patterns, a feature
> already supported in Blazegraph storage and HDT.

A third storage now meets both, on top of
[QLever](https://github.com/ad-freiburg/qlever). This document covers the half
of it that lives here.

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

The other half, and the larger one, is `passage-qlever` in the Passage
repository: this fork is a consequence of that module rather than a project
beside it, and exists so that Passage has a QLever backend. The backend itself
is documented, measured and tested there, in its `README.md` and its
`journal.md`. What belongs here is what QLever carries and what could be
offered upstream: the `OFFSET` patch above, `qlever::ScanCursor`, the JNI
bridge, and the standalone demos under `src/libqlever/jni/java/demo`.

## Architecture

```
passage-volcano  (Java)          the continuation engine, unchanged
      │  Backend<Long,String> / BackendIterator<Long,String>
      ▼
QLeverBackend, QLeverIterator    the Passage repository, passage-qlever/
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

The three classes this bridge binds — `QLeverNative`, `QLeverIterator` and
`QLeverBackend` — live in the [Passage](https://github.com/passage-org/passage)
repository, in its `passage-qlever` module. They were drafted here, next to the
bridge, and moved there once that module existed. No copy is kept here any
more: the two diverged in silence, and the copy in this tree still held two
bugs the module had already fixed — an absent IRI that matched every triple
instead of none, and an iterator that read before it advanced.

From a clone of Passage, that module builds this bridge itself:

```bash
git submodule update --init passage-qlever/qlever   # this fork, pinned
mvn -pl passage-qlever install -Pnative -DskipTests
```

Its `README.md` carries the whole recipe, including the `JAVA_HOME` and macOS
pitfalls; its `journal.md` carries what was decided along the way, what was
measured, and which conclusions later measurements reversed.

`demo/` stays in this tree: seven programs that drive the bridge from the
command line, with no Maven and no engine around them, and they are what back
the numbers quoted below. They compile against the module's jar; see
[`src/libqlever/jni/java/README.md`](src/libqlever/jni/java/README.md) for the
classpath, which `mvn dependency:build-classpath` produces.

## Running it end to end

`PassageOnQlever`, under `src/libqlever/jni/java/demo`, drives
`passage-volcano` over this bridge with one scan per execution, so that every
step yields a continuation query. It needs an index built from the graph of
Figure 2 of the paper:

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

It terminates after 6 executions with the 3 expected solutions, and the
continuation queries have the shape of Example 3.10 of the paper. Rerun on
2026-10-10 against the tree rebased onto upstream, unchanged. The process then
exits 132 rather than 0, for a reason unrelated to the evaluation that
[`src/libqlever/jni/java/README.md`](src/libqlever/jni/java/README.md)
documents, along with the classpath and the JDK it needs.

The same thing without writing any Java is in the Passage module: `passage-cli`
takes a QLever index by basename and `-r` prints each continuation. See its
`README.md`.

## What the demos verify

* `ScanCursorTest` (11 tests) covers the enumeration, `skip`, the choice of
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
* A BGP through `passage-volcano`: termination, and complete and correct
  results — `PassageOnQlever`, above.

The programs behind these numbers are in
[`src/libqlever/jni/java/demo`](src/libqlever/jni/java/demo), with a
[README](src/libqlever/jni/java/README.md) saying which index each expects.

What is *not* verified is a property of the backend rather than of this fork,
and the list lives with it, under `What is not verified` in the Passage
module's `README.md`: no workload, no data beyond memory, basic graph patterns
only, distinct scans skipped by walking rather than by blocks, no quads, no
concurrency.

## Against Blazegraph, on WatDiv 10M

Measured in the Passage module, by `QLeverVsBlazegraphTest`, and reported
there: on 10,916,457 triples the two stores answer identically, QLever reads
3.5x faster on an unbound scan, and Blazegraph joins 1.4x faster — a factor
that was ninety-five before a cache of decompressed blocks. The diagnosis of
that gap, the cache, and the block-size sweep behind it are in the module's
`README.md` under the same heading.
