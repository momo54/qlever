import fr.gdd.passage.commons.interfaces.Backend;
import fr.gdd.passage.commons.interfaces.BackendIterator;
import fr.gdd.passage.qlever.QLeverBackend;

import java.util.Set;

/**
 * Compare enumerating every object of a triple pattern with enumerating only its
 * distinct objects. The index used has 2,000,000 triples but only 1,000 distinct
 * objects, so the two differ by a factor of 2,000 in solutions emitted while
 * reading exactly the same blocks.
 */
public class DistinctBench {
    static final int OBJECT = 2;

    static <ID, VALUE> ID lookup(Backend<ID, VALUE> b, String value) {
        return b.getId(value);
    }

    static long drain(BackendIterator<Long, String> it) {
        long n = 0;
        while (it.hasNext()) { it.getId(OBJECT); it.next(); n++; }
        return n;
    }

    public static void main(String[] args) throws Exception {
        Backend<Long, String> backend = new QLeverBackend(args[0], args[1]);
        Long any = backend.any();
        Long p = lookup(backend, "<http://ex.org/p1>");

        for (int round = 0; round < 2; ++round) {
            long t0 = System.nanoTime();
            long all = drain(backend.search(any, p, any));
            long t1 = System.nanoTime();
            long distinct = drain(backend.searchDistinct(any, p, any, Set.of(OBJECT)));
            long t2 = System.nanoTime();
            if (round == 1) {
                System.out.printf("all objects      : %,9d emitted   %6.0f ms%n",
                                  all, (t1 - t0) / 1e6);
                System.out.printf("distinct objects : %,9d emitted   %6.0f ms%n",
                                  distinct, (t2 - t1) / 1e6);
            }
        }
        backend.close();
    }
}
