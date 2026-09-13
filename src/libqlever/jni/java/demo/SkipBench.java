import fr.gdd.passage.qlever.QLeverNative;
import java.util.Random;

/** Time deep skips on a 2M-triple relation, through the JNI bridge. */
public class SkipBench {
    public static void main(String[] args) {
        QLeverNative.load(args[0]);
        long backend = QLeverNative.openBackend(args[1]);
        long any = QLeverNative.any();
        long p = QLeverNative.getId(backend, "<http://ex.org/p1>");
        long cursor = QLeverNative.search(backend, any, p, any);
        long card = QLeverNative.cardinality(cursor);

        Random rng = new Random(42);   // the same offsets whichever build is measured
        long[] one = new long[3];
        int n = 200;
        // Warm up the page cache so we measure decompression, not the first read.
        for (int i = 0; i < 20; ++i) { QLeverNative.skip(cursor, rng.nextLong(card)); QLeverNative.fill(cursor, one, 1); }

        rng = new Random(42);
        long start = System.nanoTime();
        for (int i = 0; i < n; ++i) {
            QLeverNative.skip(cursor, rng.nextLong(card));
            QLeverNative.fill(cursor, one, 1);   // force the block to be materialised
        }
        double ms = (System.nanoTime() - start) / 1e6;
        System.out.printf("%d random skips over %d matches: %.1f ms total, %.3f ms per skip%n",
                          n, card, ms, ms / n);
        QLeverNative.closeCursor(cursor);
        QLeverNative.closeBackend(backend);
    }
}
