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

```bash
# The native library, from the root of the QLever checkout.
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target qleverjni qlever-index -j $(nproc)
LIB=$PWD/build/lib/libqleverjni.so        # .dylib on macOS

# The classpath of the Passage jars installed above.
CP=passage-volcano/target/passage-volcano-0.2.0.jar:passage-commons/target/passage-commons-0.2.0.jar:$(
    cd passage-volcano && mvn -q dependency:build-classpath \
        -Dmdep.outputFile=/dev/stdout -DincludeScope=compile)

# The backend and the demos.
javac -cp "$CP" -d classes fr/gdd/passage/qlever/*.java
javac -cp "$CP:classes" -d classes demo/*.java
```

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
java -cp classes                SmokeTest       "$LIB" testidx
java -cp "$CP:classes"          BackendTest     "$LIB" testidx
java -cp classes                SkipBench       "$LIB" testidx
java -cp "$CP:classes"          PassageOnQlever "$LIB" fig2
```

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
* **`PassageOnQlever`** drives `passage-volcano` with one scan per execution
  and prints every continuation query it generates. It terminates after six
  executions with the three expected solutions.
