import fr.gdd.passage.commons.interfaces.Backend;
import fr.gdd.passage.commons.interfaces.BackendIterator;
import fr.gdd.passage.qlever.QLeverBackend;

/** Drive the QLever backend through Passage's own interfaces. */
public class BackendTest {
    static int failures = 0;
    static void check(String what, boolean ok) {
        System.out.printf("%-60s %s%n", what, ok ? "OK" : "FAILED");
        if (!ok) failures++;
    }

    /**
     * Resolve an IRI through the generic interface. NOTE: Going through a type
     * variable is not a detour: `Backend.getId(String, int...)` and
     * `getId(VALUE, int...)` are ambiguous for a caller that holds a concrete
     * `Backend<Long, String>`, and Passage itself only ever calls `getId` from
     * generic code (see `BackendCache`).
     */
    static <ID, VALUE> ID lookup(Backend<ID, VALUE> b, String value) {
        return b.getId(value);
    }

    public static void main(String[] args) throws Exception {
        // Deliberately typed as the interface: this is how passage-volcano sees it.
        Backend<Long, String> backend = new QLeverBackend(args[0], args[1]);
        Long any = backend.any();
        Long p = lookup(backend, "<http://ex.org/p1>");

        BackendIterator<Long, String> it = backend.search(any, p, any);
        check("cardinality through the interface is 2,000,000",
              it.cardinality() == 2_000_000d);

        // Consume a few solutions, as a nested-loop join would.
        StringBuilder first = new StringBuilder();
        for (int i = 0; i < 3 && it.hasNext(); ++i) {
            first.append(it.getString(0)).append(" ");
            it.next();
        }
        check("iteration yields subjects: " + first.toString().trim(),
              first.toString().startsWith("<http://ex.org/s"));
        check("current() is 3 after three next()", it.current() == 3);

        // The continuation: stop, report the offset, resume on a new iterator.
        long continuation = it.current();
        String expected = it.getString(0);
        BackendIterator<Long, String> resumed = backend.search(any, p, any);
        resumed.skip(continuation);
        check("resuming at the reported offset yields the same solution",
              expected.equals(resumed.getString(0)));

        // A deep skip, which is the point of the exercise.
        long start = System.nanoTime();
        resumed.skip(1_999_990L);
        String deep = resumed.getString(0);
        long ms = (System.nanoTime() - start) / 1_000_000;
        check("skip to 1,999,990 took " + ms + "ms and yields " + deep, ms < 50);
        check("current() reports the deep offset", resumed.current() == 1_999_990L);

        // Enumerate the tail and check we reach exactly the end.
        long count = 0;
        while (resumed.hasNext()) { resumed.next(); count++; }
        check("the tail after offset 1,999,990 has 10 solutions", count == 10);

        backend.close();
        System.out.println(failures == 0 ? "\nALL CHECKS PASSED" : "\n" + failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
