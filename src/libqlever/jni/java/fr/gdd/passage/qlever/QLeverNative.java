package fr.gdd.passage.qlever;

/**
 * The native methods of the QLever JNI bridge (see `src/libqlever/jni/QleverJni.cpp`
 * in the QLever repository). Handles are opaque pointers; every `openBackend` and
 * `search` has to be paired with the corresponding `close`.
 */
public final class QLeverNative {

    private QLeverNative() {}

    /** Load the bridge from an explicit path, e.g. `.../lib/libqleverjni.dylib`. */
    public static void load(String pathToLibrary) {
        System.load(pathToLibrary);
    }

    // Backend.
    public static native long openBackend(String indexBasename);
    public static native void closeBackend(long backend);
    /** The identifier that stands for an unbound component of a triple pattern. */
    public static native long any();
    public static native long getId(long backend, String value);
    public static native String getString(long backend, long id);

    // Cursor over a single triple pattern.
    public static native long search(long backend, long s, long p, long o);
    public static native void closeCursor(long cursor);
    public static native boolean hasNext(long cursor);
    public static native void next(long cursor);
    /** `component` is 0 for the subject, 1 for the predicate, 2 for the object. */
    public static native long getComponent(long cursor, int component);
    /** Copy up to `maxTuples` matches (3 longs each) into `out`; returns the count. */
    public static native int fill(long cursor, long[] out, int maxTuples);
    public static native void skip(long cursor, long offset);
    public static native long current(long cursor);
    public static native long cardinality(long cursor);
    public static native long cardinalityEstimate(long cursor);
}
