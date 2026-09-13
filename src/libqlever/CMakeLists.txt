add_library(qlever Qlever.cpp NamedCachedQueryBlobManager.cpp
        CanonicalRowOrder.cpp NamedCacheSecondaryVocabRewriter.cpp
        ScanCursor.cpp)
qlever_target_link_libraries(qlever sparqlParser engine qlever_util index)

if (EMSCRIPTEN)
    # Build the WASM module (`qlever.mjs` + `qlever.wasm` + `qlever.d.ts"`)
    # with the embind bindings from `QleverEmscriptenBindings.cpp`.
    add_executable(qleverwasm QleverEmscriptenBindings.cpp)
    qlever_target_link_libraries(qleverwasm qlever sparqlParser engine util index)

    # The module is loaded as `qlever.mjs` (which fetches `qlever.wasm`). The
    # `.mjs` suffix marks it as an ES module for Node and bundlers alike.
    set_target_properties(qleverwasm PROPERTIES OUTPUT_NAME "qlever" SUFFIX ".mjs")

    # For a debuggable/profilable module (DWARF in the .wasm, usable with the
    # browser dev tools + the "C/C++ DevTools Support (DWARF)" extension),
    # build with `-DCMAKE_BUILD_TYPE=RelWithDebInfo` (or `Debug`). Because the
    # Conan dependencies are only built for `Release`, the install step then
    # needs the consumer-scoped build type, which reuses the Release
    # dependencies instead of rebuilding them:
    #   conan install . ... -s build_type=Release -s "&:build_type=RelWithDebInfo"

    target_link_options(qleverwasm PRIVATE
            # Generate the embind JavaScript glue code.
            -lembind
            # The module only exposes the embind bindings and has no `main`.
            --no-entry
            # Emit an ES module whose default export is the `createQleverModule()`
            # factory (`import createQleverModule from "@ad-freiburg/qlever"`).
            # MODULARIZE keeps
            # the global scope clean; EXPORT_ES6 makes the loader locate
            # `qlever.wasm` and spawn its pthread workers via `import.meta.url`,
            # which is what modern bundlers (Vite, webpack) and Node ESM expect.
            -sMODULARIZE=1
            -sEXPORT_NAME=createQleverModule
            -sEXPORT_ES6=1
            -sENVIRONMENT=web,worker,node
            # In the browser, spawning a Web Worker for multi-threading requires
            # returning to the event loop, which a blocked thread (e.g. inside
            # `buildIndex`) never does. A pre-allocated pool avoids the resulting
            # deadlocks; if it is exhausted, workers are created on demand (STRICT=0).
            -sPTHREAD_POOL_SIZE=32
            -sPTHREAD_POOL_SIZE_STRICT=0
            # Expose the virtual filesystem (to upload input data and persist
            # indices) and readable messages for uncaught C++ exceptions.
            # `EXCEPTION_STACK_TRACES` also exports the exception handling helpers
            # that `getExceptionMessage` needs.
            -sEXPORTED_RUNTIME_METHODS=FS,getExceptionMessage
            -sEXCEPTION_STACK_TRACES=1
            # Generate TypeScript definitions (`qlever.d.ts`, shipped with the npm
            # package) from the embind bindings.
            "SHELL:--emit-tsd qlever.d.ts"
    )
endif ()

# JNI bridge that exposes `ScanCursor` to the JVM (see `jni/QleverJni.cpp`).
# Only the JNI headers are needed: the resulting shared library is loaded *by* a
# JVM, so it must not link against `libjvm` itself. Built only when the headers
# are found, so that the usual build is unaffected.
find_path(QLEVER_JNI_INCLUDE_DIR jni.h
        HINTS $ENV{JAVA_HOME}/include
        $ENV{JAVA_HOME}/Contents/Home/include
        /opt/homebrew/opt/openjdk/include
        /usr/lib/jvm/default-java/include)
if (QLEVER_JNI_INCLUDE_DIR AND NOT EMSCRIPTEN)
    add_library(qleverjni SHARED jni/QleverJni.cpp)
    # `jni.h` includes the platform header from a sibling directory
    # (`darwin/jni_md.h` or `linux/jni_md.h`); add both if present.
    target_include_directories(qleverjni SYSTEM PRIVATE
            ${QLEVER_JNI_INCLUDE_DIR}
            ${QLEVER_JNI_INCLUDE_DIR}/darwin
            ${QLEVER_JNI_INCLUDE_DIR}/linux)
    qlever_target_link_libraries(qleverjni qlever sparqlParser engine qlever_util index)
    message(STATUS "Building the QLever JNI bridge, headers from ${QLEVER_JNI_INCLUDE_DIR}")
else ()
    message(STATUS "No JNI headers found; skipping the QLever JNI bridge")
endif ()
