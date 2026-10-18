# The Java side of the QLever JNI bridge

The [Passage](https://github.com/passage-org/passage) backend over a QLever
index was drafted here, next to the bridge it binds, and now lives in the
Passage repository, in its `passage-qlever` module:

| | |
| --- | --- |
| `QLeverNative` | the native methods of `../QleverJni.cpp`; no dependencies |
| `QLeverIterator` | `BackendIterator<Long, String>`, batching matches across JNI |
| `QLeverBackend` | `Backend<Long, String>` |

No copy of those three is kept in this tree any more. The two diverged without
a word, and the copy here still had an absent IRI matching every triple
instead of none, and an iterator that read before it advanced — both of them
fixed in the module a week after this copy was last touched. Compile against
the module, never against a copy.

What stays here is `demo/`: the programs that were used to verify the chain,
and they are what back the numbers quoted in
[PASSAGE-QLEVER-BACKEND.md](../../../../PASSAGE-QLEVER-BACKEND.md); run them to
reproduce those numbers rather than taking them on trust.

## Building

`passage-qlever`, `passage-commons` and `passage-volcano` are **not published
to any Maven repository** — there is nothing under the `fr.gdd` group on Maven
Central — so they have to be built from source and installed into the local
repository. `passage-qlever` is the module that holds the three classes, so it
is the one the demos here compile against:

```bash
export JAVA_HOME=$(brew --prefix openjdk@21)   # or wherever a 21 lives
git clone https://github.com/momo54/passage.git
cd passage
git checkout filter-continuation   # the branch that carries passage-qlever
mvn -pl passage-qlever,passage-volcano -am install -DskipTests
```

That produces `fr.gdd:passage-qlever:0.2.0`, `passage-commons:0.2.0` and
`passage-volcano:0.2.0` in `~/.m2`, along with `jena-utils`, `raw` and
`passage-blazegraph`. `passage-qlever` has not been merged upstream, which is
why the clone above is the Nantes fork and not `passage-org/passage`.

`JAVA_HOME` is load-bearing. Passage targets release 21, so a Maven running
under a 17 — which a version manager pinned to an older default gives you —
stops at the first module with `Failed to execute goal
...maven-compiler-plugin:compile on project jena-utils: error: release version
21 not supported`. Setting `javac` for the classes below is not enough; this is
Maven's own JDK.

The checkout is not optional caution. `0.2.0` is the version `master` carries
rather than a release, so it moves, and the jar records nothing about where it
came from — its manifest holds only `Created-By` and `Java-Version`. So an
existing `0.2.0` in `~/.m2` cannot be checked against a commit, and installing
from `master` later overwrites it in place without a word. Two machines can
therefore disagree on what `0.2.0` means, which is also why the step above has
to run at least once per machine: nothing in this repository can supply those
jars, and no `~/.m2` may be assumed to hold them.

The resulting jars are class file version 65, so compiling the classes below
against a 17 fails in turn, with `class file has wrong version 65.0, should be
61.0`. Hence the explicit `$JDK` everywhere from here on.

The pin is worth trusting: installing `8d38952` into an empty local repository
(`mvn ... -Dmaven.repo.local=/tmp/m2`, which leaves `~/.m2` alone) produces
`passage-commons` and `passage-volcano` whose 77 and 83 class files are
byte-for-byte identical to an install made from that commit months earlier. So
while `0.2.0` moves, that commit's `0.2.0` does not.

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

The classpath is every jar `passage-volcano` and `passage-qlever` pull in. The
clone above is the only thing that can put them in `~/.m2`, but it is not
needed again afterwards to recompute the classpath, which matters because the
clone is usually gone by the time anyone reruns this. A pom that depends on
nothing else resolves it from whatever `~/.m2` already holds:

```bash
cat > pom.xml <<'EOF'
<project xmlns="http://maven.apache.org/POM/4.0.0"><modelVersion>4.0.0</modelVersion>
  <groupId>local</groupId><artifactId>cp</artifactId><version>0</version>
  <dependencies>
    <dependency><groupId>fr.gdd</groupId>
      <artifactId>passage-volcano</artifactId><version>0.2.0</version>
    </dependency>
    <dependency><groupId>fr.gdd</groupId>
      <artifactId>passage-qlever</artifactId><version>0.2.0</version>
    </dependency>
  </dependencies></project>
EOF
mvn -q dependency:build-classpath -Dmdep.outputFile=cp.txt
CP=$(cat cp.txt)
```

`mvn -o` is not enough for that: resolving the plugin itself needs the network
once.

```bash
$JDK/javac -cp "$CP" -d classes demo/*.java
```

NOTE: the braces in `"${CP}:classes"` are not decoration. In zsh, which is the
default shell on macOS, `"$CP:classes"` applies the `:c` history modifier and
silently yields `...jarlasses`, so the last entry is mangled and the class is
not found. Bash leaves it alone, which is why this reads as a working command
and is not one.

`SmokeTest` loads only `QLeverNative`, which has no dependencies of its own,
but it comes off the `passage-qlever` jar like the rest — so every command
below carries the classpath, where two of them used to need only `classes`.

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
[PASSAGE-QLEVER-BACKEND.md](../../../../PASSAGE-QLEVER-BACKEND.md#running-it-end-to-end).

## Running them

```bash
$JDK/java -cp "${CP}:classes"    SmokeTest       "$LIB" testidx
$JDK/java -cp "${CP}:classes"    BackendTest     "$LIB" testidx
$JDK/java -cp "${CP}:classes"    SkipBench       "$LIB" testidx
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
  exhaustion past the end. Twelve checks, and it touches no Passage type.
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
