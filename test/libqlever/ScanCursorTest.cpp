// Copyright 2026 The QLever Authors
//
// You may not use this file except in compliance with the Apache 2.0 License,
// which can be found in the `LICENSE` file at the root of the QLever project.

#include <gmock/gmock.h>

#include <string>
#include <vector>

#include "../util/IndexTestHelpers.h"
#include "libqlever/ScanCursor.h"
#include "parser/TripleComponent.h"

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
