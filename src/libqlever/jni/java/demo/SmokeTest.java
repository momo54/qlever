import fr.gdd.passage.qlever.QLeverNative;

/** End-to-end check of the QLever JNI bridge against a real index. */
public class SmokeTest {

    static int failures = 0;

    static void check(String what, boolean ok) {
        System.out.printf("%-58s %s%n", what, ok ? "OK" : "FAILED");
        if (!ok) failures++;
    }

    public static void main(String[] args) {
        QLeverNative.load(args[0]);
        long backend = QLeverNative.openBackend(args[1]);
        long any = QLeverNative.any();

        long p = QLeverNative.getId(backend, "<http://ex.org/p1>");
        check("predicate <p1> is in the vocabulary", p != any);
        check("an unknown IRI resolves to `any`",
              QLeverNative.getId(backend, "<http://ex.org/nope>") == any);

        long cursor = QLeverNative.search(backend, any, p, any);
        long card = QLeverNative.cardinality(cursor);
        check("cardinality of `?s <p1> ?o` is 2,000,000", card == 2_000_000L);
        check("a fresh cursor is at offset 0", QLeverNative.current(cursor) == 0);

        // Bulk read, which is how a real backend consumes a scan.
        long[] buffer = new long[3 * 4];
        int n = QLeverNative.fill(cursor, buffer, 4);
        check("fill() returns 4 matches", n == 4);
        check("the predicate of every match is <p1>",
              buffer[1] == p && buffer[4] == p && buffer[7] == p && buffer[10] == p);
        check("fill() advanced the cursor by 4", QLeverNative.current(cursor) == 4);
        String firstSubject = QLeverNative.getString(backend, buffer[0]);
        check("subjects resolve back to IRIs: " + firstSubject,
              firstSubject.startsWith("<http://ex.org/s"));

        // The skip that the whole exercise is about.
        long target = 1_999_990L;
        long start = System.nanoTime();
        QLeverNative.skip(cursor, target);
        long elapsedMs = (System.nanoTime() - start) / 1_000_000;
        check("skip(1,999,990) reports that offset",
              QLeverNative.current(cursor) == target);
        check("skip took less than 50ms (was " + elapsedMs + "ms)", elapsedMs < 50);

        // Round trip: the offset reported by one cursor positions another one.
        long[] afterSkip = new long[3];
        QLeverNative.fill(cursor, afterSkip, 1);
        long cursor2 = QLeverNative.search(backend, any, p, any);
        QLeverNative.skip(cursor2, target);
        long[] fresh = new long[3];
        QLeverNative.fill(cursor2, fresh, 1);
        check("a fresh cursor skipped to the same offset yields the same match",
              afterSkip[0] == fresh[0] && afterSkip[2] == fresh[2]);

        // Exhaustion.
        QLeverNative.skip(cursor2, 2_000_000L);
        check("skipping past the end exhausts the cursor",
              !QLeverNative.hasNext(cursor2));

        QLeverNative.closeCursor(cursor);
        QLeverNative.closeCursor(cursor2);
        QLeverNative.closeBackend(backend);
        System.out.println(failures == 0 ? "\nALL CHECKS PASSED" : "\n" + failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
