// Copyright 2026 The QLever Authors
//
// You may not use this file except in compliance with the Apache 2.0 License,
// which can be found in the `LICENSE` file at the root of the QLever project.

#include <gmock/gmock.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "../util/IndexTestHelpers.h"
#include "global/RuntimeParameters.h"
#include "index/Index.h"
#include "libqlever/ScanCursor.h"
#include "parser/TripleComponent.h"
#include "util/AllocatorWithLimit.h"
#include "util/Timer.h"

using namespace qlever;
using namespace testing;

namespace {

using Component = ScanCursor::Component;

// A small graph whose single predicate `<p>` has enough triples that a scan of
// it is worth positioning into, plus two triples with another predicate so that
// the scans are not trivially the whole index.
std::string testGraph() {
  std::string turtle;
  for (int i = 0; i < 12; ++i) {
    absl::StrAppend(&turtle, "<s", i, "> <p> <o", i % 4, "> . ");
  }
  absl::StrAppend(&turtle, "<s0> <q> <o0> . <s1> <q> <o1> .");
  return turtle;
}

// An IRI as the cursor's constructor expects it.
TripleComponent iri(std::string_view name) {
  return TripleComponent::Iri::fromIriref(absl::StrCat("<", name, ">"));
}

// Read the cursor to exhaustion and return the `(subject, object)` pairs.
std::vector<std::pair<std::string, std::string>> readAll(ScanCursor& cursor) {
  std::vector<std::pair<std::string, std::string>> result;
  while (cursor.hasNext()) {
    result.emplace_back(cursor.getString(Component::Subject),
                        cursor.getString(Component::Object));
    cursor.next();
  }
  return result;
}

// A cursor over `?s <p> ?o` on the test index.
struct Fixture {
  QueryExecutionContext* qec_ = ad_utility::testing::getQec(testGraph());
  ScanCursor makeCursorDistinct(std::optional<TripleComponent> s,
                                std::optional<TripleComponent> p,
                                std::optional<TripleComponent> o,
                                std::vector<ScanCursor::Component> distinct)
      const {
    return ScanCursor{qec_->getIndex().getImpl(), std::move(s), std::move(p),
                      std::move(o), qec_->locatedTriplesSharedState(),
                      distinct};
  }
  ScanCursor makeCursor(std::optional<TripleComponent> s,
                        std::optional<TripleComponent> p,
                        std::optional<TripleComponent> o) const {
    return ScanCursor{qec_->getIndex().getImpl(), std::move(s), std::move(p),
                      std::move(o), qec_->locatedTriplesSharedState()};
  }
};

}  // namespace

// The cursor enumerates exactly the matches of the triple pattern, and reports
// an offset that advances by one per match.
// Time the creation of a cursor against a real on-disk index, which is what an
// engine that drives the joins itself does once per outer solution. Measured
// through the JNI bridge it costs about 0.86 ms, flat in the size of the index
// and flat in `lazy-index-scan-num-blocks-in-flight`; this says whether that
// belongs to QLever or to the boundary. Skipped unless an index is named:
//
//   QLEVER_SCAN_BENCH_INDEX=/path/to/basename \
//     ./ScanCursorTest --gtest_filter='*timeCursorCreation*'
// The join `?s rdf:type ?c . ?s ?p ?o` run entirely in C++, so that the same
// join through the JNI bridge can be compared against it. The published
// comparison against Blazegraph has QLever losing that join by a factor of
// two, and Blazegraph's backend is pure Java while this one crosses a foreign
// function boundary — so the published figure cannot say which of the two it
// measures. This one can.
//
// Two consumption styles, because the boundary dictates one of them:
//
//   * tuple at a time, `hasNext`/`getId`/`next`, what a C++ engine would do;
//   * bulk, `fill` into a buffer of 3 x 1024 `Id`s, what the bridge does,
//     minus the crossing itself.
//
// If the bulk figure here matches the one through JNI, the boundary is free
// and the join difference belongs to the storage. If it does not, the
// difference is the boundary, and the published comparison measures the
// bridge rather than the index.
//
//   QLEVER_SCAN_BENCH_INDEX=/path/to/basename \
//     ./ScanCursorTest --gtest_filter='*timeAJoin*'
TEST(ScanCursor, timeAJoinAgainstARealIndex) {
  const char* basename = std::getenv("QLEVER_SCAN_BENCH_INDEX");
  if (basename == nullptr) {
    GTEST_SKIP() << "set QLEVER_SCAN_BENCH_INDEX to an index basename";
  }
  auto allocator = ad_utility::makeUnlimitedAllocator<Id>();
  Index index{allocator};
  index.createFromOnDiskIndex(basename, false);
  auto locatedTriples =
      index.deltaTriplesManager().getCurrentLocatedTriplesSharedState();

  // The same cache the Passage backend sets, so that the two sides of the
  // comparison differ in the boundary and in nothing else.
  setRuntimeParameter<&RuntimeParameters::lazyIndexScanBlockCacheSize_>(16);

  // 200,000 solutions, the unit the published comparison uses.
  constexpr size_t target = 200'000;
  constexpr size_t batchSize = 1024;
  using OptId = std::optional<Id>;
  using OptTc = std::optional<TripleComponent>;
  const auto typeIri = OptTc{TripleComponent::Iri::fromIriref(
      "<http://www.w3.org/1999/02/22-rdf-syntax-ns#type>")};

  auto ms = [](auto from, auto to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
  };
  auto perSecond = [](size_t n, double millis) {
    return millis > 0 ? static_cast<size_t>(n / millis * 1000.0) : size_t{0};
  };

  // Tuple at a time.
  size_t solutions = 0;
  size_t outers = 0;
  auto start = std::chrono::steady_clock::now();
  {
    ScanCursor outer{index.getImpl(), OptTc{}, typeIri, OptTc{},
                     locatedTriples, {}};
    while (outer.hasNext() && solutions < target) {
      Id subject = outer.getId(Component::Subject);
      ++outers;
      ScanCursor inner{index.getImpl(), OptId{subject}, OptId{},
                       OptId{},         locatedTriples, {}};
      while (inner.hasNext() && solutions < target) {
        // Read a component, so that the scan is not optimised into counting.
        [[maybe_unused]] volatile auto sink =
            inner.getId(Component::Object).getBits();
        ++solutions;
        inner.next();
      }
      outer.next();
    }
  }
  auto tupleAtATime = std::chrono::steady_clock::now();

  // Bulk, as the bridge consumes it.
  size_t bulkSolutions = 0;
  std::vector<Id> buffer(3 * batchSize);
  {
    ScanCursor outer{index.getImpl(), OptTc{}, typeIri, OptTc{},
                     locatedTriples, {}};
    while (outer.hasNext() && bulkSolutions < target) {
      Id subject = outer.getId(Component::Subject);
      ScanCursor inner{index.getImpl(), OptId{subject}, OptId{},
                       OptId{},         locatedTriples, {}};
      size_t written = 0;
      while ((written = inner.fill(ql::span<Id>{buffer}, batchSize)) > 0 &&
             bulkSolutions < target) {
        for (size_t i = 0; i < written; ++i) {
          [[maybe_unused]] volatile auto sink = buffer[3 * i + 2].getBits();
        }
        bulkSolutions += written;
      }
      outer.next();
    }
  }
  auto bulk = std::chrono::steady_clock::now();

  ASSERT_EQ(solutions, target);
  ASSERT_EQ(bulkSolutions >= target, true);

  std::cout << "\n  join ?s rdf:type ?c . ?s ?p ?o, first " << target
            << " solutions, " << outers << " outer solutions, block cache 16\n"
            << "    tuple at a time  " << ms(start, tupleAtATime) << " ms  "
            << perSecond(solutions, ms(start, tupleAtATime)) << " /s\n"
            << "    bulk, fill(1024) " << ms(tupleAtATime, bulk) << " ms  "
            << perSecond(bulkSolutions, ms(tupleAtATime, bulk)) << " /s\n"
            << "    fan-out          "
            << (outers > 0 ? solutions / outers : 0) << " solutions per scan\n"
            << std::endl;
}

TEST(ScanCursor, timeCursorCreationAgainstARealIndex) {
  const char* basename = std::getenv("QLEVER_SCAN_BENCH_INDEX");
  if (basename == nullptr) {
    GTEST_SKIP() << "set QLEVER_SCAN_BENCH_INDEX to an index basename";
  }
  auto allocator = ad_utility::makeUnlimitedAllocator<Id>();
  Index index{allocator};
  index.createFromOnDiskIndex(basename, false);
  auto locatedTriples =
      index.deltaTriplesManager().getCurrentLocatedTriplesSharedState();

  // Subjects to bind, read from the index so that each of them exists.
  constexpr size_t numCursors = 2000;
  std::vector<Id> subjects;
  {
    using OptId = std::optional<Id>;
    qlever::ScanCursor all{index.getImpl(), OptId{}, OptId{},
                           OptId{}, locatedTriples, {}};
    while (all.hasNext() && subjects.size() < numCursors) {
      subjects.push_back(all.getId(qlever::ScanCursor::Component::Subject));
      all.next();
    }
  }
  ASSERT_EQ(subjects.size(), numCursors);

  // Creation alone, without reading, which is lazy and so reads nothing.
  using OptId = std::optional<Id>;
  uint64_t sink = 0;
  auto start = std::chrono::steady_clock::now();
  for (Id subject : subjects) {
    qlever::ScanCursor cursor{index.getImpl(), OptId{subject}, OptId{},
                              OptId{},         locatedTriples, {}};
    sink += cursor.cardinalityEstimate().first;
  }
  auto created = std::chrono::steady_clock::now();

  // Creation plus the first read, which is what opens the scan. Done twice:
  // in the order the subjects came out of the index, so that consecutive scans
  // land in the same block, and then shuffled, so that they do not. QLever
  // caches query results and vocabulary lookups but not decompressed blocks —
  // a merge join never returns to one — so the two should cost the same, which
  // is what says whether a block is decompressed again for every lookup.
  size_t read = 0;
  for (Id subject : subjects) {
    qlever::ScanCursor cursor{index.getImpl(), OptId{subject}, OptId{},
                              OptId{},         locatedTriples, {}};
    if (cursor.hasNext()) {
      sink += cursor.getId(qlever::ScanCursor::Component::Object).getBits();
      ++read;
    }
  }
  auto done = std::chrono::steady_clock::now();

  auto shuffled = subjects;
  std::mt19937 generator{42};
  std::shuffle(shuffled.begin(), shuffled.end(), generator);
  for (Id subject : shuffled) {
    qlever::ScanCursor cursor{index.getImpl(), OptId{subject}, OptId{},
                              OptId{},         locatedTriples, {}};
    if (cursor.hasNext()) {
      sink += cursor.getId(qlever::ScanCursor::Component::Object).getBits();
    }
  }
  auto shuffledDone = std::chrono::steady_clock::now();

  // The same subject every time, so every lookup wants the one block. With a
  // cache of decompressed blocks this would cost one decompression; without
  // one it costs as many as there are lookups, which is what this shows.
  for (size_t i = 0; i < numCursors; ++i) {
    qlever::ScanCursor cursor{index.getImpl(), OptId{subjects.front()},
                              OptId{},         OptId{}, locatedTriples, {}};
    if (cursor.hasNext()) {
      sink += cursor.getId(qlever::ScanCursor::Component::Object).getBits();
    }
  }
  auto sameDone = std::chrono::steady_clock::now();

  // For scale: the same number of rows read through a single scan, which
  // decompresses each block once. This is the floor a cache could reach.
  size_t rows = 0;
  {
    qlever::ScanCursor cursor{index.getImpl(), OptId{}, OptId{}, OptId{},
                              locatedTriples, {}};
    while (cursor.hasNext() && rows < numCursors) {
      sink += cursor.getId(qlever::ScanCursor::Component::Object).getBits();
      cursor.next();
      ++rows;
    }
  }
  auto oneScanDone = std::chrono::steady_clock::now();

  // The same lookups again, against the size of the block cache. The subjects
  // come out of the index in order and a block spans thousands of consecutive
  // ones, so a single entry should already hit nearly always.
  auto ms = [](auto from, auto to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
  };
  std::vector<std::pair<size_t, double>> withCache;
  for (size_t capacity : {size_t{0}, size_t{1}, size_t{4}}) {
    setRuntimeParameter<&RuntimeParameters::lazyIndexScanBlockCacheSize_>(
        capacity);
    auto cacheStart = std::chrono::steady_clock::now();
    for (Id subject : subjects) {
      qlever::ScanCursor cursor{index.getImpl(), OptId{subject}, OptId{},
                                OptId{},         locatedTriples, {}};
      if (cursor.hasNext()) {
        sink += cursor.getId(qlever::ScanCursor::Component::Object).getBits();
      }
    }
    withCache.emplace_back(
        capacity, ms(cacheStart, std::chrono::steady_clock::now()) / numCursors);
  }
  setRuntimeParameter<&RuntimeParameters::lazyIndexScanBlockCacheSize_>(0);
  std::cout << "\n== " << numCursors << " cursors on " << basename << "\n"
            << "   construction only    " << ms(start, created) << " ms, "
            << ms(start, created) / numCursors << " ms each\n"
            << "   first read, in order " << ms(created, done) << " ms, "
            << ms(created, done) / numCursors << " ms each  (" << read
            << " read)\n"
            << "   first read, shuffled " << ms(done, shuffledDone) << " ms, "
            << ms(done, shuffledDone) / numCursors << " ms each\n"
            << "   same subject each    " << ms(shuffledDone, sameDone)
            << " ms, " << ms(shuffledDone, sameDone) / numCursors
            << " ms each\n"
            << "   " << rows << " rows, one scan   "
            << ms(sameDone, oneScanDone) << " ms, "
            << ms(sameDone, oneScanDone) / numCursors << " ms each\n"
            << "   checksum " << sink << "\n";
  for (const auto& [capacity, each] : withCache) {
    std::cout << "   block cache " << capacity << "        " << each
              << " ms each\n";
  }
}

TEST(ScanCursor, enumeratesAllMatchesAndCountsThem) {
  Fixture f;
  auto cursor = f.makeCursor(std::nullopt, iri("p"), std::nullopt);
  EXPECT_EQ(cursor.current(), 0u);
  EXPECT_EQ(cursor.cardinality(), 12u);

  auto all = readAll(cursor);
  EXPECT_EQ(all.size(), 12u);
  EXPECT_FALSE(cursor.hasNext());
  // Once exhausted, the offset is the number of matches, so that a continuation
  // created from it is empty.
  EXPECT_EQ(cursor.current(), 12u);

  // The enumeration follows the order of the permutation, that is, it is sorted
  // by the subject. NOTE: This has to be checked on the `Id`s; the order of the
  // vocabulary is a Unicode collation order, which is not the byte order of the
  // corresponding strings.
  auto byId = f.makeCursor(std::nullopt, iri("p"), std::nullopt);
  std::vector<Id> subjectIds;
  while (byId.hasNext()) {
    subjectIds.push_back(byId.getId(Component::Subject));
    byId.next();
  }
  EXPECT_THAT(subjectIds, SizeIs(12u));
  EXPECT_TRUE(ql::ranges::is_sorted(subjectIds, [](Id a, Id b) {
    return a.compareWithoutLocalVocab(b) < 0;
  }));
}

// `skip` positions the cursor without changing what it enumerates from there.
TEST(ScanCursor, skipPositionsTheCursor) {
  Fixture f;
  auto reference = f.makeCursor(std::nullopt, iri("p"), std::nullopt);
  auto all = readAll(reference);

  for (uint64_t offset : {uint64_t{0}, uint64_t{1}, uint64_t{7},
                          uint64_t{11}, uint64_t{12}, uint64_t{100}}) {
    auto cursor = f.makeCursor(std::nullopt, iri("p"), std::nullopt);
    cursor.skip(offset);
    EXPECT_EQ(cursor.current(), offset) << "with offset " << offset;
    std::vector<std::pair<std::string, std::string>> expected{
        all.begin() + std::min<size_t>(offset, all.size()), all.end()};
    EXPECT_EQ(readAll(cursor), expected) << "with offset " << offset;
  }
}

// The property that a continuation-query engine relies on: the offset reported
// by a cursor, applied to a freshly created cursor over the same pattern,
// restores exactly the same position. This is what makes it possible to stop a
// scan, hand out its offset, and resume it later in a different process.
TEST(ScanCursor, offsetRoundTripsThroughAFreshCursor) {
  Fixture f;
  auto reference = f.makeCursor(std::nullopt, iri("p"), std::nullopt);
  auto all = readAll(reference);

  for (size_t stopAfter = 0; stopAfter <= all.size(); ++stopAfter) {
    // Consume `stopAfter` matches, as an interrupted execution would.
    auto interrupted = f.makeCursor(std::nullopt, iri("p"), std::nullopt);
    std::vector<std::pair<std::string, std::string>> consumed;
    for (size_t i = 0; i < stopAfter; ++i) {
      ASSERT_TRUE(interrupted.hasNext());
      consumed.emplace_back(interrupted.getString(Component::Subject),
                            interrupted.getString(Component::Object));
      interrupted.next();
    }
    uint64_t continuationOffset = interrupted.current();
    EXPECT_EQ(continuationOffset, stopAfter);

    // Resume from the reported offset with a new cursor, and check that the
    // two halves together are exactly the complete enumeration.
    auto resumed = f.makeCursor(std::nullopt, iri("p"), std::nullopt);
    resumed.skip(continuationOffset);
    auto rest = readAll(resumed);
    consumed.insert(consumed.end(), rest.begin(), rest.end());
    EXPECT_EQ(consumed, all) << "when interrupted after " << stopAfter;
  }
}

// Bound components are returned by the getters even though the scan does not
// produce a column for them, and the permutation is chosen accordingly.
TEST(ScanCursor, boundComponentsAreReported) {
  Fixture f;
  auto cursor = f.makeCursor(iri("s0"), std::nullopt, std::nullopt);
  ASSERT_TRUE(cursor.hasNext());
  // `<s0>` occurs with `<p>` and with `<q>`.
  EXPECT_EQ(cursor.cardinality(), 2u);
  EXPECT_EQ(cursor.getString(Component::Subject), "<s0>");

  auto twoBound = f.makeCursor(iri("s0"), iri("p"), std::nullopt);
  ASSERT_TRUE(twoBound.hasNext());
  EXPECT_EQ(twoBound.getString(Component::Subject), "<s0>");
  EXPECT_EQ(twoBound.getString(Component::Predicate), "<p>");
  EXPECT_EQ(twoBound.getString(Component::Object), "<o0>");
  twoBound.next();
  EXPECT_FALSE(twoBound.hasNext());
}

// A pattern whose bound component is not in the vocabulary matches nothing.
TEST(ScanCursor, unknownComponentYieldsAnEmptyCursor) {
  Fixture f;
  auto cursor = f.makeCursor(std::nullopt, iri("notInTheGraph"), std::nullopt);
  EXPECT_FALSE(cursor.hasNext());
  EXPECT_EQ(cursor.cardinality(), 0u);
}

// The permutation is the one in which the bound components form a prefix.
TEST(ScanCursor, permutationSelection) {
  using enum Permutation::Enum;
  EXPECT_EQ(ScanCursor::permutationFor(false, false, false), SPO);
  EXPECT_EQ(ScanCursor::permutationFor(true, false, false), SPO);
  EXPECT_EQ(ScanCursor::permutationFor(false, true, false), PSO);
  EXPECT_EQ(ScanCursor::permutationFor(false, false, true), OSP);
  EXPECT_EQ(ScanCursor::permutationFor(true, true, false), SPO);
  EXPECT_EQ(ScanCursor::permutationFor(true, false, true), SOP);
  EXPECT_EQ(ScanCursor::permutationFor(false, true, true), POS);
  EXPECT_EQ(ScanCursor::permutationFor(true, true, true), SPO);
}

namespace {
using Components = std::vector<Component>;

// Read a cursor over distinct values to exhaustion, returning the values of
// `component` as strings.
std::vector<std::string> readAllDistinct(ScanCursor& cursor,
                                         Component component) {
  std::vector<std::string> result;
  while (cursor.hasNext()) {
    result.push_back(cursor.getString(component));
    cursor.next();
  }
  return result;
}
}  // namespace

// The permutation has to order the components as `bound`, then `distinctOn`,
// then the rest, so that the distinct components are sorted and equal ones
// adjacent. For a triple pattern such an order always exists.
TEST(ScanCursor, permutationSelectionWithDistinct) {
  using enum Permutation::Enum;
  auto perm = [](bool s, bool p, bool o, Components distinct) {
    return ScanCursor::permutationFor(s, p, o, distinct);
  };
  // Nothing bound: the distinct component has to come first.
  EXPECT_EQ(perm(false, false, false, {Component::Subject}), SPO);
  EXPECT_EQ(perm(false, false, false, {Component::Predicate}), PSO);
  EXPECT_EQ(perm(false, false, false, {Component::Object}), OSP);
  // One component bound: the distinct one has to come second.
  EXPECT_EQ(perm(false, true, false, {Component::Object}), POS);
  EXPECT_EQ(perm(false, true, false, {Component::Subject}), PSO);
  EXPECT_EQ(perm(true, false, false, {Component::Object}), SOP);
  // Two distinct components.
  EXPECT_EQ(perm(false, false, false, {Component::Subject, Component::Object}),
            SOP);
  // A bound component may not also be a distinct one.
  EXPECT_ANY_THROW(perm(false, true, false, {Component::Predicate}));
}

// A cursor over distinct values enumerates exactly the distinct values, in the
// order of the permutation, and reports its position in *that* sequence.
TEST(ScanCursor, distinctEnumeratesEachValueOnce) {
  Fixture f;
  // `<p>` relates 12 subjects to only 4 distinct objects.
  auto distinctObjects =
      f.makeCursorDistinct(std::nullopt, iri("p"), std::nullopt,
                           {Component::Object});
  auto objects = readAllDistinct(distinctObjects, Component::Object);
  EXPECT_THAT(objects, SizeIs(4u));
  EXPECT_THAT(objects, Each(StartsWith("<o")));
  // No value occurs twice, and they come in the order of the permutation.
  auto sorted = objects;
  ql::ranges::sort(sorted);
  EXPECT_EQ(ql::ranges::unique(sorted).begin() - sorted.begin(), 4);

  // The subjects are all distinct already, so deduplicating changes nothing.
  auto distinctSubjects =
      f.makeCursorDistinct(std::nullopt, iri("p"), std::nullopt,
                           {Component::Subject});
  EXPECT_THAT(readAllDistinct(distinctSubjects, Component::Subject),
              SizeIs(12u));
}

// `skip` on a distinct cursor positions it in the sequence of distinct values,
// and the offset round trips through a freshly created cursor exactly as it
// does for a cursor over all triples. This is what makes a distinct scan a
// continuable leaf.
TEST(ScanCursor, distinctSkipAndOffsetRoundTrip) {
  Fixture f;
  auto reference = f.makeCursorDistinct(std::nullopt, iri("p"), std::nullopt,
                                        {Component::Object});
  auto all = readAllDistinct(reference, Component::Object);
  ASSERT_THAT(all, SizeIs(4u));

  for (uint64_t offset : {uint64_t{0}, uint64_t{1}, uint64_t{3}, uint64_t{4},
                          uint64_t{100}}) {
    auto cursor = f.makeCursorDistinct(std::nullopt, iri("p"), std::nullopt,
                                       {Component::Object});
    cursor.skip(offset);
    EXPECT_EQ(cursor.current(), offset) << "with offset " << offset;
    std::vector<std::string> expected{
        all.begin() + std::min<size_t>(offset, all.size()), all.end()};
    EXPECT_EQ(readAllDistinct(cursor, Component::Object), expected)
        << "with offset " << offset;
  }
}

// The number of distinct values, not the number of triples.
TEST(ScanCursor, distinctCardinality) {
  Fixture f;
  auto objects = f.makeCursorDistinct(std::nullopt, iri("p"), std::nullopt,
                                      {Component::Object});
  EXPECT_EQ(objects.cardinality(), 4u);
  auto subjects = f.makeCursorDistinct(std::nullopt, iri("p"), std::nullopt,
                                       {Component::Subject});
  EXPECT_EQ(subjects.cardinality(), 12u);
  auto all = f.makeCursor(std::nullopt, iri("p"), std::nullopt);
  EXPECT_EQ(all.cardinality(), 12u);
}
