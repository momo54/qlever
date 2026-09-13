// Copyright 2026 The QLever Authors
//
// You may not use this file except in compliance with the Apache 2.0 License,
// which can be found in the `LICENSE` file at the root of the QLever project.

// JNI bridge that exposes QLever's index at the level of a single triple
// pattern scan, which is what an engine that drives the joins itself needs (see
// `libqlever/ScanCursor.h`). The Java side of it is
// `fr.gdd.passage.qlever.QLeverBackend` and `…QLeverIterator`.
//
// Objects are handed to Java as opaque `long` handles; every `open`/`search`
// has to be paired with the corresponding `close`, which the Java classes do in
// their `close()` methods.

#include <jni.h>

#include <memory>
#include <new>
#include <string>
#include <vector>

#include "index/Index.h"
#include "index/IndexImpl.h"
#include "index/ExportIds.h"
#include "libqlever/ScanCursor.h"
#include "util/AllocatorWithLimit.h"

namespace {

// The Java `long` that stands for an unbound component of a triple pattern.
// `Id`s are 64 bit, so no valid `Id` may collide with it; the sentinel is the
// bit pattern of an `Undefined` `Id`, which never occurs in an index.
jlong anySentinel() {
  return static_cast<jlong>(Id::makeUndefined().getBits());
}

// Everything that a scan needs to stay alive, so that the Java side only has to
// juggle two kinds of handle.
struct BackendHandle {
  ad_utility::AllocatorWithLimit<Id> allocator_;
  Index index_;
  LocatedTriplesSharedState locatedTriples_;

  explicit BackendHandle(const std::string& indexBasename)
      : allocator_{ad_utility::makeUnlimitedAllocator<Id>()},
        index_{allocator_} {
    index_.createFromOnDiskIndex(indexBasename, false);
    locatedTriples_ =
        index_.deltaTriplesManager().getCurrentLocatedTriplesSharedState();
  }
};

template <typename T>
T* fromHandle(jlong handle) {
  return reinterpret_cast<T*>(handle);
}

// Convert `value` to an optional `Id`, mapping the `any` sentinel to
// `std::nullopt`.
std::optional<Id> toOptionalId(jlong value) {
  if (value == anySentinel()) {
    return std::nullopt;
  }
  return Id::fromBits(static_cast<uint64_t>(value));
}

// Run `f`, translating a C++ exception into a Java `RuntimeException`. Returns
// `onError` in that case, because a JNI method has to return a value even when
// it raises.
template <typename F>
auto guarded(JNIEnv* env, F f, decltype(f()) onError) -> decltype(f()) {
  try {
    return f();
  } catch (const std::exception& e) {
    env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    return onError;
  } catch (...) {
    env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                  "unknown C++ exception in the QLever JNI bridge");
    return onError;
  }
}

}  // namespace

extern "C" {

// ___________________________________________________________________ backend

JNIEXPORT jlong JNICALL Java_fr_gdd_passage_qlever_QLeverNative_openBackend(
    JNIEnv* env, jclass, jstring indexBasename) {
  return guarded(
      env,
      [&]() -> jlong {
        const char* chars = env->GetStringUTFChars(indexBasename, nullptr);
        std::string basename{chars};
        env->ReleaseStringUTFChars(indexBasename, chars);
        return reinterpret_cast<jlong>(new BackendHandle{basename});
      },
      0);
}

JNIEXPORT void JNICALL Java_fr_gdd_passage_qlever_QLeverNative_closeBackend(
    JNIEnv*, jclass, jlong backend) {
  delete fromHandle<BackendHandle>(backend);
}

JNIEXPORT jlong JNICALL Java_fr_gdd_passage_qlever_QLeverNative_any(JNIEnv*,
                                                                   jclass) {
  return anySentinel();
}

// Look up `value` in the vocabulary. Returns the `any` sentinel if the value
// does not occur in the index, which makes any scan using it empty.
JNIEXPORT jlong JNICALL Java_fr_gdd_passage_qlever_QLeverNative_getId(
    JNIEnv* env, jclass, jlong backend, jstring value) {
  return guarded(
      env,
      [&]() -> jlong {
        const char* chars = env->GetStringUTFChars(value, nullptr);
        std::string asString{chars};
        env->ReleaseStringUTFChars(value, chars);
        const auto& index = fromHandle<BackendHandle>(backend)->index_;
        VocabIndex vocabIndex;
        if (!index.getVocab().getId(asString, &vocabIndex)) {
          return anySentinel();
        }
        return static_cast<jlong>(
            Id::makeFromVocabIndex(vocabIndex).getBits());
      },
      anySentinel());
}

JNIEXPORT jstring JNICALL Java_fr_gdd_passage_qlever_QLeverNative_getString(
    JNIEnv* env, jclass, jlong backend, jlong id) {
  return guarded(
      env,
      [&]() -> jstring {
        auto* handle = fromHandle<BackendHandle>(backend);
        // Reuse the cursor's conversion by going through a one-off cursor is
        // wasteful; instead resolve the `Id` directly.
        auto literalOrIri = ql::exportIds::idToLiteralOrIri(
            handle->index_.getImpl(),
            Id::fromBits(static_cast<uint64_t>(id)), LocalVocab{});
        std::string result =
            literalOrIri.has_value()
                ? std::string{
                      std::move(literalOrIri).value().toStringRepresentation()}
                : std::string{};
        return env->NewStringUTF(result.c_str());
      },
      nullptr);
}

// ____________________________________________________________________ cursor

JNIEXPORT jlong JNICALL Java_fr_gdd_passage_qlever_QLeverNative_search(
    JNIEnv* env, jclass, jlong backend, jlong s, jlong p, jlong o) {
  return guarded(
      env,
      [&]() -> jlong {
        auto* handle = fromHandle<BackendHandle>(backend);
        return reinterpret_cast<jlong>(new qlever::ScanCursor{
            handle->index_.getImpl(), toOptionalId(s), toOptionalId(p),
            toOptionalId(o), handle->locatedTriples_});
      },
      0);
}

JNIEXPORT void JNICALL Java_fr_gdd_passage_qlever_QLeverNative_closeCursor(
    JNIEnv*, jclass, jlong cursor) {
  delete fromHandle<qlever::ScanCursor>(cursor);
}

JNIEXPORT jboolean JNICALL Java_fr_gdd_passage_qlever_QLeverNative_hasNext(
    JNIEnv* env, jclass, jlong cursor) {
  return guarded(
      env,
      [&]() -> jboolean {
        return fromHandle<qlever::ScanCursor>(cursor)->hasNext() ? JNI_TRUE
                                                                 : JNI_FALSE;
      },
      JNI_FALSE);
}

JNIEXPORT void JNICALL Java_fr_gdd_passage_qlever_QLeverNative_next(
    JNIEnv* env, jclass, jlong cursor) {
  guarded(
      env,
      [&]() {
        fromHandle<qlever::ScanCursor>(cursor)->next();
        return 0;
      },
      0);
}

JNIEXPORT jlong JNICALL Java_fr_gdd_passage_qlever_QLeverNative_getComponent(
    JNIEnv* env, jclass, jlong cursor, jint component) {
  return guarded(
      env,
      [&]() -> jlong {
        auto asComponent =
            static_cast<qlever::ScanCursor::Component>(component);
        return static_cast<jlong>(fromHandle<qlever::ScanCursor>(cursor)
                                      ->getId(asComponent)
                                      .getBits());
      },
      anySentinel());
}

// Copy up to `maxTuples` matches into `out` (three `long`s per match) and
// advance the cursor past them. This is what makes a scan usable from Java:
// crossing the JNI boundary once per match would dominate everything else.
JNIEXPORT jint JNICALL Java_fr_gdd_passage_qlever_QLeverNative_fill(
    JNIEnv* env, jclass, jlong cursor, jlongArray out, jint maxTuples) {
  return guarded(
      env,
      [&]() -> jint {
        auto length = static_cast<size_t>(env->GetArrayLength(out));
        std::vector<Id> buffer(length);
        size_t written = fromHandle<qlever::ScanCursor>(cursor)->fill(
            ql::span<Id>{buffer}, static_cast<size_t>(maxTuples));
        // `Id` and `jlong` are both 64 bit, but not layout-compatible as far as
        // the standard is concerned, so convert explicitly.
        std::vector<jlong> asLongs(3 * written);
        for (size_t i = 0; i < asLongs.size(); ++i) {
          asLongs[i] = static_cast<jlong>(buffer[i].getBits());
        }
        env->SetLongArrayRegion(out, 0, static_cast<jsize>(asLongs.size()),
                                asLongs.data());
        return static_cast<jint>(written);
      },
      0);
}

JNIEXPORT void JNICALL Java_fr_gdd_passage_qlever_QLeverNative_skip(
    JNIEnv* env, jclass, jlong cursor, jlong offset) {
  guarded(
      env,
      [&]() {
        fromHandle<qlever::ScanCursor>(cursor)->skip(
            static_cast<uint64_t>(offset));
        return 0;
      },
      0);
}

JNIEXPORT jlong JNICALL Java_fr_gdd_passage_qlever_QLeverNative_current(
    JNIEnv* env, jclass, jlong cursor) {
  return guarded(
      env,
      [&]() -> jlong {
        return static_cast<jlong>(
            fromHandle<qlever::ScanCursor>(cursor)->current());
      },
      0);
}

JNIEXPORT jlong JNICALL Java_fr_gdd_passage_qlever_QLeverNative_cardinality(
    JNIEnv* env, jclass, jlong cursor) {
  return guarded(
      env,
      [&]() -> jlong {
        return static_cast<jlong>(
            fromHandle<qlever::ScanCursor>(cursor)->cardinality());
      },
      0);
}

JNIEXPORT jlong JNICALL
Java_fr_gdd_passage_qlever_QLeverNative_cardinalityEstimate(JNIEnv* env, jclass,
                                                            jlong cursor) {
  return guarded(
      env,
      [&]() -> jlong {
        return static_cast<jlong>(fromHandle<qlever::ScanCursor>(cursor)
                                      ->cardinalityEstimate()
                                      .second);
      },
      0);
}

}  // extern "C"
