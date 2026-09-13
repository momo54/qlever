import fr.gdd.passage.qlever.QLeverNative;
import java.util.Random;

/**
 * Time the pattern that Passage actually uses: `Backend.search` takes no offset,
 * so a scan is created and only then skipped into position (see `PassageScan`).
 */
public class SearchThenSkipBench {
    public static void main(String[] args) {
        QLeverNative.load(args[0]);
        long backend = QLeverNative.openBackend(args[1]);
        long any = QLeverNative.any();
        long p = QLeverNative.getId(backend, "<http://ex.org/p1>");
        long probe = QLeverNative.search(backend, any, p, any);
        long card = QLeverNative.cardinality(probe);
        QLeverNative.closeCursor(probe);

        long[] one = new long[3];
        int n = 200;
        for (int warm = 0; warm < 2; ++warm) {
            Random rng = new Random(42);
            long start = System.nanoTime();
            for (int i = 0; i < n; ++i) {
                long c = QLeverNative.search(backend, any, p, any);  // no offset
                QLeverNative.skip(c, rng.nextLong(card));            // then position
                QLeverNative.fill(c, one, 1);
                QLeverNative.closeCursor(c);
            }
            double ms = (System.nanoTime() - start) / 1e6;
            if (warm == 1) {
                System.out.printf("%d x (search + skip + read): %.1f ms total, %.3f ms each%n",
                                  n, ms, ms / n);
            }
        }
        QLeverNative.closeBackend(backend);
    }
}
