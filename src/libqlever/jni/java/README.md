# The Java side of the QLever JNI bridge

`fr/gdd/passage/qlever/` holds a [Passage](https://github.com/passage-org/passage)
backend over a QLever index. It belongs in a `passage-qlever` module of the
Passage repository and lives here next to the bridge it binds; move it there as
it is.

| | |
| --- | --- |
| `QLeverNative` | the native methods of `../QleverJni.cpp`; no dependencies |
| `QLeverIterator` | `BackendIterator<Long, String>`, batching matches across JNI |
| `QLeverBackend` | `Backend<Long, String>` |

`demo/` holds the programs that were used to verify the chain. They are what
back the numbers quoted in [PASSAGE-QLEVER-BACKEND.md](../../../../PASSAGE-QLEVER-BACKEND.md);
run them to reproduce those numbers rather than taking them on trust.

## Building

`passage-commons` and `passage-volcano` are **not published to any Maven
repository** — there is nothing under the `fr.gdd` group on Maven Central — so
they have to be built from source and installed into the local repository:

```bash
git clone https://github.com/passage-org/passage.git
cd passage && mvn -pl passage-volcano -am install -DskipTests
```

That produces `fr.gdd:passage-commons:0.2.0` and `fr.gdd:passage-volcano:0.2.0`
in `~/.m2`. NOTE: `0.2.0` is the version carried by the project's `master`, not
a released one, so it moves; the backend here was written and verified against
commit `8d38952`.

Those jars are compiled for **Java 21** (class file version 65). Building
against a 17 fails with `class file has wrong version 65.0, should be 61.0`,
which is easy to hit when a version manager pins an older default, so point at
a 21 explicitly.

On macOS the `cmake` line below needs the flags from
[`PASSAGE-README.md`](../../../../PASSAGE-README.md#1-build), which also lists
what a Homebrew `abseil` and a recent Apple Clang break.

```bash
# The native library, from the root of the QLever checkout.
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target qleverjni qlever-index -j $(nproc)
LIB=$PWD/build/lib/libqleverjni.so        # .dylib on macOS

JDK=$(brew --prefix openjdk@21)/bin       # or wherever a 21 lives
```

The classpath is every jar `passage-volcano` pulls in. Once the jars are in
`~/.m2` the clone above is no longer needed to recover it, which matters
because the clone is usually gone by the time anyone reruns this. A pom that
depends on nothing else resolves it:

```bash
cat > pom.xml <<'EOF'
<project xmlns="http://maven.apache.org/POM/4.0.0"><modelVersion>4.0.0</modelVersion>
  <groupId>local</groupId><artifactId>cp</artifactId><version>0</version>
  <dependencies><dependency><groupId>fr.gdd</groupId>
    <artifactId>passage-volcano</artifactId><version>0.2.0</version>
  </dependency></dependencies></project>
EOF
mvn -q dependency:build-classpath -Dmdep.outputFile=cp.txt
CP=$(cat cp.txt)
```

`mvn -o` is not enough for that: resolving the plugin itself needs the network
once.

```bash
$JDK/javac -cp "$CP" -d classes fr/gdd/passage/qlever/*.java
$JDK/javac -cp "${CP}:classes" -d classes demo/*.java
```

NOTE: the braces in `"${CP}:classes"` are not decoration. In zsh, which is the
default shell on macOS, `"$CP:classes"` applies the `:c` history modifier and
silently yields `...jarlasses`, so the last entry is mangled and the class is
not found. Bash leaves it alone, which is why this reads as a working command
and is not one.

`SmokeTest` needs neither Passage jar, only `QLeverNative`.

## The two indexes the demos expect

`SmokeTest`, `BackendTest` and `SkipBench` run against an index with two
million triples sharing one predicate, so that a single relation spans many
blocks:

```bash
awk 'BEGIN{for(i=0;i<2000000;i++) printf "<http://ex.org/s%d> <http://ex.org/p1> <http://ex.org/o%d> .\n", i, i%1000}' > data.ttl
build/qlever-index -i testidx -f data.ttl -F ttl -p true
```

`PassageOnQlever` runs against the graph of Figure 2 of the Passage paper; the
six triples are listed in
[PASSAGE-QLEVER-BACKEND.md](../../../../PASSAGE-QLEVER-BACKEND.md#running-it).

## Running them

```bash
$JDK/java -cp classes            SmokeTest       "$LIB" testidx
$JDK/java -cp "${CP}:classes"    BackendTest     "$LIB" testidx
$JDK/java -cp classes            SkipBench       "$LIB" testidx
$JDK/java -cp "${CP}:classes"    PassageOnQlever "$LIB" fig2
```

NOTE: the JVM dies with a `SIGSEGV` **after** a demo has printed its results
and its verdict, so the exit status is 132 rather than 0 and cannot be used to
decide whether a run succeeded. The crash is in jemalloc's per-thread cleanup
as one of QLever's own worker threads exits:

```
_pthread_exit -> _pthread_tsd_cleanup -> je_tsd_cleanup
  -> tcache_destroy -> tcache_flush_cache -> je_arena_ptr_array_flush
```

`link_libraries()` in the top-level `CMakeLists.txt` is directory-scoped, so
jemalloc reaches every target created after it, `qleverjni` included, and
through `qlever`, `engine` and `index` transitively as well — filtering it off
the one target therefore does not help, and `MALLOC_CONF` is not read. A
library meant to be loaded into a JVM that already has an allocator should not
link it at all, which is a change to a line this fork does not own. Until then,
read the verdict the demo prints and ignore the status.

* **`SmokeTest`** exercises the native methods directly: the vocabulary, a bulk
  read, a deep `skip`, the round trip of an offset through a fresh cursor, and
  exhaustion past the end. Twelve checks, no Passage dependency.
* **`BackendTest`** does the same through `Backend<Long, String>` and
  `BackendIterator<Long, String>`, that is, through the types
  `passage-volcano` sees.
* **`SearchThenSkipBench`** times the pattern that Passage actually uses:
  `Backend.search` takes no offset, so a scan is created and only then skipped
  into position. This is what the cursor's laziness is for; against a cursor
  that opens its scan eagerly it costs 0.764 ms per scan instead of 0.416 ms,
  because the first block is decompressed only to be discarded by the `skip`.
* **`SkipBench`** times 200 random deep skips on a single cursor. Run it once as built, then again
  against a build with the block skipping reverted
  (`git revert` the commit described in
  [PASSAGE-README.md](../../../../PASSAGE-README.md)) to reproduce the
  comparison; the seed is fixed, so both runs use the same offsets.
* **`DistinctTest`** exercises `Backend.searchDistinct` through the interface on
  the two million triple index, which has 2,000,000 subjects but only 1,000
  distinct objects: the counts for each of the three positions, and the round
  trip of an offset that counts distinct values.
* **`DistinctBench`** times enumerating every object of a triple pattern against
  enumerating only its distinct objects, on an index where those differ by a
  factor of 2,000. Both read the same blocks; what the distinct scan saves is
  the solutions it does not hand across the boundary.
* **`PassageOnQlever`** drives `passage-volcano` with one scan per execution
  and prints every continuation query it generates. It terminates after six
  executions with the three expected solutions.
