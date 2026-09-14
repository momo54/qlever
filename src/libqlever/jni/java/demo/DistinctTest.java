import fr.gdd.passage.commons.interfaces.Backend;
import fr.gdd.passage.commons.interfaces.BackendIterator;
import fr.gdd.passage.qlever.QLeverBackend;

import java.util.Set;

/**
 * Exercise `Backend.searchDistinct` on a two million triple index whose single
 * predicate relates 2,000,000 subjects to only 1,000 distinct objects.
 */
public class DistinctTest {
    static final int SUBJECT = 0, PREDICATE = 1, OBJECT = 2;
    static int failures = 0;

    static void check(String what, boolean ok) {
        System.out.printf("%-62s %s%n", what, ok ? "OK" : "FAILED");
        if (!ok) failures++;
    }

    static <ID, VALUE> ID lookup(Backend<ID, VALUE> b, String value) {
        return b.getId(value);   // see BackendTest for why this goes through a type variable
    }

    public static void main(String[] args) throws Exception {
        Backend<Long, String> backend = new QLeverBackend(args[0], args[1]);
        Long any = backend.any();
        Long p = lookup(backend, "<http://ex.org/p1>");

        // 1,000 distinct objects behind 2,000,000 triples.
        BackendIterator<Long, String> objects =
                backend.searchDistinct(any, p, any, Set.of(OBJECT));
        long count = 0;
        while (objects.hasNext()) { objects.next(); count++; }
        check("distinct objects of `?s <p1> ?o` is 1,000 (was " + count + ")",
              count == 1_000);

        // Every subject is distinct, so deduplicating changes nothing.
        BackendIterator<Long, String> subjects =
                backend.searchDistinct(any, p, any, Set.of(SUBJECT));
        check("distinct subjects is 2,000,000",
              subjects.cardinality() == 2_000_000d);

        // A single predicate in the whole index.
        BackendIterator<Long, String> predicates =
                backend.searchDistinct(any, any, any, Set.of(PREDICATE));
        long numPredicates = 0;
        while (predicates.hasNext()) { predicates.next(); numPredicates++; }
        check("distinct predicates of `?s ?p ?o` is 1 (was " + numPredicates + ")",
              numPredicates == 1);

        // The offset counts distinct values, and round trips through a fresh
        // iterator: this is what makes a distinct scan a continuable leaf.
        BackendIterator<Long, String> reference =
                backend.searchDistinct(any, p, any, Set.of(OBJECT));
        reference.skip(400);
        String at400 = reference.getString(OBJECT);
        check("skip(400) reports offset 400", reference.current() == 400);

        BackendIterator<Long, String> resumed =
                backend.searchDistinct(any, p, any, Set.of(OBJECT));
        resumed.skip(reference.current());
        check("a fresh distinct iterator resumes at the same value: " + at400,
              at400.equals(resumed.getString(OBJECT)));

        // And the tail is exactly what remains.
        long tail = 0;
        while (resumed.hasNext()) { resumed.next(); tail++; }
        check("the tail after offset 400 has 600 values (was " + tail + ")",
              tail == 600);

        backend.close();
        System.out.println(failures == 0 ? "\nALL CHECKS PASSED" : "\n" + failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
