// Copyright 2026 The QLever Authors
//
// You may not use this file except in compliance with the Apache 2.0 License,
// which can be found in the `LICENSE` file at the root of the QLever project.

#ifndef QLEVER_SRC_LIBQLEVER_SCANCURSOR_H
#define QLEVER_SRC_LIBQLEVER_SCANCURSOR_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "engine/idTable/IdTable.h"
#include "backports/span.h"
#include "global/Id.h"
#include "index/CompressedRelation.h"
#include "index/DeltaTriples.h"
#include "index/Permutation.h"
#include "index/ScanSpecification.h"
#include "parser/TripleComponent.h"
#include "util/CancellationHandle.h"

class IndexImpl;

namespace qlever {

// A tuple-at-a-time cursor over the matches of a single triple pattern.
//
// In contrast to the query-level interface of `Qlever`, this exposes the index
// at the level of a single scan, which is what an external engine that drives
// the joins itself needs (for example a continuation-query engine, which has to
// suspend a scan, report the offset it reached, and later resume another scan
// at that offset).
//
// The cursor is deliberately kept close to what such engines require:
//
//   * `skip(offset)` positions the cursor at an arbitrary offset without
//     reading the triples that are skipped over, and
//   * `current()` reports the offset of the cursor, so that a caller can later
//     recreate a cursor positioned at exactly the same place.
//
// The order of the matches is the order of the underlying permutation. It is
// deterministic and stable for as long as the index and the located triples
// that the cursor was created with do not change, which is what makes the
// offsets reported by `current()` meaningful across cursors.
//
// NOTE: A cursor keeps the `LocatedTriplesSharedState` it was created with
// alive, so a sequence of cursors that is meant to enumerate a triple pattern
// exactly once has to be created from the same snapshot. Otherwise an update
// that happens in between shifts the offsets.
class ScanCursor {
 public:
  // The components of a triple, used to address them in `getId` and friends.
  enum struct Component : uint8_t { Subject = 0, Predicate = 1, Object = 2 };

  // Create a cursor over the triples matching (`subject`, `predicate`,
  // `object`), where `std::nullopt` means "any". The permutation is chosen
  // from the set of bound components (see `permutationFor`).
  //
  // If one of the bound components does not occur in the index's vocabulary,
  // the cursor is immediately exhausted (`hasNext() == false`).
  ScanCursor(const IndexImpl& index, std::optional<TripleComponent> subject,
             std::optional<TripleComponent> predicate,
             std::optional<TripleComponent> object,
             LocatedTriplesSharedState locatedTriples);

  // Same, but with the components already resolved to `Id`s, as an engine that
  // caches the `Id`s of the constants of its query would have them.
  ScanCursor(const IndexImpl& index, std::optional<Id> subject,
             std::optional<Id> predicate, std::optional<Id> object,
             LocatedTriplesSharedState locatedTriples);

  // Copy up to `maxTuples` matches into `out`, three `Id`s (subject,
  // predicate, object) per match, and advance the cursor past them. Returns
  // the number of matches written. This is the bulk counterpart of
  // `hasNext`/`next`/`getId`, for callers for which a single match at a time is
  // too small a unit of work (for example across a foreign-function boundary).
  size_t fill(ql::span<Id> out, size_t maxTuples);

  ~ScanCursor();
  ScanCursor(ScanCursor&&) noexcept;
  ScanCursor& operator=(ScanCursor&&) noexcept;
  ScanCursor(const ScanCursor&) = delete;
  ScanCursor& operator=(const ScanCursor&) = delete;

  // Return true iff the cursor currently points at a match, that is, iff the
  // getters below may be called.
  bool hasNext() const;

  // Advance the cursor to the next match. May only be called when `hasNext()`.
  void next();

  // The `component` of the match that the cursor currently points at. May only
  // be called when `hasNext()`.
  Id getId(Component component) const;

  // The `component` of the current match, as it would be written in a SPARQL
  // query. May only be called when `hasNext()`.
  std::string getString(Component component) const;

  // Position the cursor at the match with index `offset` (zero-based), skipping
  // over the preceding matches without reading them where the index metadata
  // allows it (see `seekToOffset` in `CompressedRelation.cpp`). An `offset`
  // beyond the last match leaves the cursor exhausted.
  void skip(uint64_t offset);

  // The offset of the match that the cursor currently points at, or the number
  // of matches if it is exhausted. Creating a new cursor for the same triple
  // pattern and calling `skip(current())` on it restores this position.
  uint64_t current() const { return current_; }

  // The offset of the previous match, or 0 if the cursor never advanced.
  uint64_t previous() const { return current_ == 0 ? 0 : current_ - 1; }

  // Position the cursor back at the first match.
  void reset() { skip(0); }

  // The exact number of matches. NOTE: This reads those blocks whose metadata
  // is not sufficient to determine their contribution, so it is not free.
  uint64_t cardinality() const;

  // A lower and an upper bound for the number of matches, computed from the
  // index metadata alone.
  std::pair<uint64_t, uint64_t> cardinalityEstimate() const;

  // The permutation that a triple pattern with the given bound components is
  // scanned in. The bound components have to form a prefix of the permutation,
  // which this mapping guarantees. Exposed for testing.
  static Permutation::Enum permutationFor(bool subjectIsBound,
                                          bool predicateIsBound,
                                          bool objectIsBound);

 private:
  // Shared part of the two constructors, called once the components have been
  // resolved to `Id`s and reordered into the order of the permutation.
  void initialize(const IndexImpl& index, ScanSpecification spec);

  // Discard the current scan and start a new one at `offset`.
  void restartAt(uint64_t offset);

  // Make `block_` point at a block that still has an unread row, or clear it if
  // the scan is exhausted.
  void advanceToNextNonEmptyBlock();

  const IndexImpl* index_;
  LocatedTriplesSharedState locatedTriples_;
  Permutation::Enum permutation_;
  // The permutation applied to `(subject, predicate, object)`, that is,
  // `keyOrder_[i]` is the component stored in column `i`.
  qlever::KeyOrder keyOrder_;
  // The `Id`s of the bound components, in the order of the permutation. Its
  // size is the number of columns that the scan does *not* return.
  std::vector<Id> boundIds_;
  // `std::nullopt` if one of the bound components is not in the vocabulary, in
  // which case the cursor is empty.
  std::optional<ScanSpecification> scanSpec_;
  std::optional<Permutation::ScanSpecAndBlocks> scanSpecAndBlocks_;
  ad_utility::SharedCancellationHandle cancellationHandle_;

  // The currently open scan, and the block plus row that the cursor points at.
  std::optional<CompressedRelationReader::IdTableGeneratorInputRange> blocks_;
  std::optional<IdTable> block_;
  size_t rowInBlock_ = 0;
  uint64_t current_ = 0;
};

}  // namespace qlever

#endif  // QLEVER_SRC_LIBQLEVER_SCANCURSOR_H
