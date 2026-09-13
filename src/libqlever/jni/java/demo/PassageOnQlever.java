import com.google.common.collect.ConcurrentHashMultiset;
import com.google.common.collect.Multiset;
import fr.gdd.passage.commons.generics.BackendBindings;
import fr.gdd.passage.qlever.QLeverBackend;
import fr.gdd.passage.volcano.PassageExecutionContext;
import fr.gdd.passage.volcano.PassageExecutionContextBuilder;
import fr.gdd.passage.volcano.push.PassagePushExecutor;
import fr.gdd.passage.volcano.transforms.Quad2Pattern;
import org.apache.jena.query.QueryFactory;
import org.apache.jena.sparql.algebra.Algebra;
import org.apache.jena.sparql.algebra.Op;
import org.apache.jena.sparql.algebra.OpAsQuery;

import java.util.Objects;

/**
 * Run a real SPARQL query through passage-volcano on top of a QLever index, with
 * one scan per execution so that every step yields a continuation query. This is
 * the graph of Figure 2 of the Passage paper (WWW'25).
 */
public class PassageOnQlever {

    public static void main(String[] args) {
        QLeverBackend backend = new QLeverBackend(args[0], args[1]);

        String query = """
                SELECT * WHERE {
                  ?human <http://ex.org/type> <http://ex.org/Human> .
                  ?human <http://ex.org/occupation> ?occupation
                }""";

        Multiset<BackendBindings<?, ?>> results = ConcurrentHashMultiset.create();
        int step = 0;
        String current = query;
        while (Objects.nonNull(current)) {
            PassageExecutionContextBuilder<Long, String> builder =
                    new PassageExecutionContextBuilder<Long, String>()
                            .setName("PUSH")
                            .setMaxScans(1L)           // one scan per execution
                            .setMaxParallel(1)
                            .setBackend(backend)
                            .setExecutorFactory(ec ->
                                    new PassagePushExecutor<>((PassageExecutionContext<?, ?>) ec));

            System.out.printf("%n--- step %d, query sent to the engine ---%n%s%n", step, current);
            PassageExecutionContext<Long, String> context = builder.build();
            Op op = Algebra.compile(QueryFactory.create(current));
            int before = results.size();
            Op paused = context.executor.execute(op, results::add);
            System.out.printf("    -> %d new solution(s)%n", results.size() - before);
            current = Objects.nonNull(paused)
                    ? OpAsQuery.asQuery(new Quad2Pattern().visit(paused)).toString()
                    : null;
            step += 1;
            if (step > 50) { throw new IllegalStateException("did not terminate"); }
        }

        System.out.printf("%n=== terminated after %d executions, %d solutions ===%n", step, results.size());
        results.forEach(b -> System.out.println("    " + b));
        backend.close();

        if (results.size() != 3) {
            System.out.println("UNEXPECTED: expected 3 solutions");
            System.exit(1);
        }
        System.out.println("\nOK: complete and correct results through continuations");
    }
}
