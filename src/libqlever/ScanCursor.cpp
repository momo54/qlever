// Copyright 2026 The QLever Authors
//
// You may not use this file except in compliance with the Apache 2.0 License,
// which can be found in the `LICENSE` file at the root of the QLever project.

#include "libqlever/ScanCursor.h"

#include "index/ExportIds.h"
#include "index/IndexImpl.h"

namespace qlever {

namespace {
// The `Permutation::Enum` whose `KeyOrder` is `keys`, which has to be a
// permutation of `(0, 1, 2)`.
Permutation::Enum permutationWithKeyOrder(const std::array<uint8_t, 3>& keys) {
  using enum Permutation::Enum;
  static constexpr std::array<std::pair<std::array<uint8_t, 3>,
                                        Permutation::Enum>, 6>
      all{{{{0, 1, 2}, SPO}, {{0, 2, 1}, SOP}, {{1, 0, 2}, PSO},
           {{1, 2, 0}, POS}, {{2, 0, 1}, OSP}, {{2, 1, 0}, OPS}}};
  auto it = ql::ranges::find(all, keys, &std::pair<std::array<uint8_t, 3>,
                                                   Permutation::Enum>::first);
  AD_CORRECTNESS_CHECK(it != all.end());
  return it->second;
}
}  // namespace

// _____________________________________________________________________________
Permutation::Enum ScanCursor::permutationFor(
    bool subjectIsBound, bool predicateIsBound, bool objectIsBound,
    ql::span<const ScanCursor::Component> distinctOn) {
  std::array<bool, 3> isBound{subjectIsBound, predicateIsBound, objectIsBound};
  std::array<bool, 3> isDistinct{false, false, false};
  for (auto component : distinctOn) {
    auto index = static_cast<size_t>(component);
    AD_CONTRACT_CHECK(!isBound.at(index),
                      "a bound component cannot also be a distinct one");
    AD_CONTRACT_CHECK(!isDistinct.at(index),
                      "a component may occur at most once in `distinctOn`");
    isDistinct.at(index) = true;
  }
  // Bound components first, then the distinct ones, then the rest. Within each
  // of the three groups the order is the canonical one, so that the order in
  // which a pattern is enumerated depends only on the pattern and on
  // `distinctOn`, never on a query plan.
  std::array<uint8_t, 3> keys{};
  size_t next = 0;
  auto append = [&keys, &next](const std::array<bool, 3>& selected) {
    for (uint8_t component = 0; component < 3; ++component) {
      if (selected.at(component)) {
        keys.at(next++) = component;
      }
    }
  };
  append(isBound);
  append(isDistinct);
  std::array<bool, 3> isRest{};
  for (size_t i = 0; i < 3; ++i) {
    isRest.at(i) = !isBound.at(i) && !isDistinct.at(i);
  }
  append(isRest);
  AD_CORRECTNESS_CHECK(next == 3);
  return permutationWithKeyOrder(keys);
}

// _____________________________________________________________________________
ScanCursor::ScanCursor(const IndexImpl& index,
                       std::optional<TripleComponent> subject,
                       std::optional<TripleComponent> predicate,
                       std::optional<TripleComponent> object,
                       LocatedTriplesSharedState locatedTriples,
                       ql::span<const Component> distinctOn)
    : index_{&index},
      locatedTriples_{std::move(locatedTriples)},
      permutation_{permutationFor(subject.has_value(), predicate.has_value(),
                                  object.has_value(), distinctOn)},
      keyOrder_{Permutation::toKeyOrder(permutation_)},
      cancellationHandle_{
          std::make_shared<ad_utility::CancellationHandle<>>()} {
  // Reorder the components into the order of the permutation. Because the
  // bound components are a prefix of it, this also puts them first.
  std::array<std::optional<TripleComponent>, 3> spo{
      std::move(subject), std::move(predicate), std::move(object)};
  std::array<std::optional<TripleComponent>, 3> cols;
  for (size_t i = 0; i < 3; ++i) {
    cols.at(i) = std::move(spo.at(keyOrder_.keys().at(i)));
  }
  AD_CORRECTNESS_CHECK(
      !(cols[0].has_value() && !cols[1].has_value() && cols[2].has_value()),
      "the bound components of a scan have to be a prefix of the permutation");

  numDistinctComponents_ = distinctOn.size();
  distinctOn_.assign(distinctOn.begin(), distinctOn.end());
  initialize(index, ScanSpecificationAsTripleComponent{cols[0], cols[1],
                                                       cols[2]}
                        .toScanSpecification(index));
}

// _____________________________________________________________________________
ScanCursor::ScanCursor(const IndexImpl& index, std::optional<Id> subject,
                       std::optional<Id> predicate, std::optional<Id> object,
                       LocatedTriplesSharedState locatedTriples,
                       ql::span<const Component> distinctOn)
    : index_{&index},
      locatedTriples_{std::move(locatedTriples)},
      permutation_{permutationFor(subject.has_value(), predicate.has_value(),
                                  object.has_value(), distinctOn)},
      keyOrder_{Permutation::toKeyOrder(permutation_)},
      cancellationHandle_{
          std::make_shared<ad_utility::CancellationHandle<>>()} {
  std::array<std::optional<Id>, 3> spo{subject, predicate, object};
  std::array<std::optional<Id>, 3> cols;
  for (size_t i = 0; i < 3; ++i) {
    cols.at(i) = spo.at(keyOrder_.keys().at(i));
  }
  numDistinctComponents_ = distinctOn.size();
  distinctOn_.assign(distinctOn.begin(), distinctOn.end());
  initialize(index, ScanSpecification{cols[0], cols[1], cols[2]});
}

// _____________________________________________________________________________
void ScanCursor::initialize(const IndexImpl& index, ScanSpecification spec) {
  // The bound components are a prefix of the permutation; remember their `Id`s,
  // because the scan does not produce a column for them but `getId` still has
  // to return them.
  for (const auto& optId : {spec.col0Id(), spec.col1Id(), spec.col2Id()}) {
    if (!optId.has_value()) {
      break;
    }
    boundIds_.push_back(optId.value());
  }
  // Remember the bound `Id`s indexed by component rather than by column, so
  // that an equivalent cursor can be rebuilt (see `cardinality`).
  {
    std::array<std::optional<Id>, 3> perColumn{spec.col0Id(), spec.col1Id(),
                                               spec.col2Id()};
    for (size_t column = 0; column < 3; ++column) {
      boundIdPerComponent_.at(keyOrder_.keys().at(column)) =
          perColumn.at(column);
    }
  }
  scanSpec_ = std::move(spec);
  scanSpecAndBlocks_ = index.getPermutation(permutation_)
                           .getScanSpecAndBlocks(scanSpec_.value(),
                                                 *locatedTriples_);
  positionAt(0);
}

// _____________________________________________________________________________
size_t ScanCursor::fill(ql::span<Id> out, size_t maxTuples) {
  size_t numTuples = std::min(maxTuples, out.size() / 3);
  size_t written = 0;
  while (written < numTuples && hasNext()) {
    out[3 * written] = getId(Component::Subject);
    out[3 * written + 1] = getId(Component::Predicate);
    out[3 * written + 2] = getId(Component::Object);
    ++written;
    next();
  }
  return written;
}

// _____________________________________________________________________________
ScanCursor::~ScanCursor() = default;
ScanCursor::ScanCursor(ScanCursor&&) noexcept = default;
ScanCursor& ScanCursor::operator=(ScanCursor&&) noexcept = default;

// _____________________________________________________________________________
void ScanCursor::positionAt(uint64_t offset) {
  current_ = offset;
  block_.reset();
  blocks_.reset();
  rowInBlock_ = 0;
  started_ = false;
}

// _____________________________________________________________________________
void ScanCursor::ensureStarted() {
  if (started_) {
    return;
  }
  started_ = true;
  blocks_ = index_->getPermutation(permutation_)
                .lazyScan(scanSpecAndBlocks_.value(), std::nullopt, {},
                          cancellationHandle_, *locatedTriples_,
                          LimitOffsetClause{std::nullopt, current_});
  advanceToNextNonEmptyBlock();
}

// _____________________________________________________________________________
void ScanCursor::advanceToNextNonEmptyBlock() {
  while (!block_.has_value() || rowInBlock_ >= block_.value().numRows()) {
    auto next = blocks_.value().get();
    if (!next.has_value()) {
      block_.reset();
      return;
    }
    block_ = std::move(next.value());
    rowInBlock_ = 0;
  }
}

// _____________________________________________________________________________
bool ScanCursor::hasNext() {
  ensureStarted();
  return block_.has_value();
}

// _____________________________________________________________________________
void ScanCursor::advanceOneTriple() {
  ++rowInBlock_;
  advanceToNextNonEmptyBlock();
}

// _____________________________________________________________________________
bool ScanCursor::currentPrefixEquals(ql::span<const Id> prefix) {
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (block_.value().at(rowInBlock_, i) != prefix[i]) {
      return false;
    }
  }
  return true;
}

// _____________________________________________________________________________
void ScanCursor::next() {
  AD_CONTRACT_CHECK(hasNext());
  if (numDistinctComponents_ == 0) {
    advanceOneTriple();
    ++current_;
    return;
  }
  // The distinct components are the leading columns of the scan, so their
  // values are sorted and equal ones adjacent: skipping the rest of the group
  // is a matter of advancing while they do not change.
  currentPrefix_.clear();
  for (size_t i = 0; i < numDistinctComponents_; ++i) {
    currentPrefix_.push_back(block_.value().at(rowInBlock_, i));
  }
  do {
    advanceOneTriple();
  } while (block_.has_value() && currentPrefixEquals(currentPrefix_));
  ++current_;
}

// _____________________________________________________________________________
Id ScanCursor::getId(Component component) {
  AD_CONTRACT_CHECK(hasNext());
  // Find the column of the permutation that stores the requested component.
  auto asIndex = static_cast<uint8_t>(component);
  const auto& keys = keyOrder_.keys();
  size_t column = 0;
  while (column < 3 && keys.at(column) != asIndex) {
    ++column;
  }
  AD_CORRECTNESS_CHECK(column < 3);
  // The scan only returns the columns that are not bound; the bound ones come
  // first and are stored in `boundIds_`.
  if (column < boundIds_.size()) {
    return boundIds_.at(column);
  }
  return block_.value().at(rowInBlock_, column - boundIds_.size());
}

// _____________________________________________________________________________
std::string ScanCursor::getString(Component component) {
  static const LocalVocab emptyLocalVocab{};
  auto literalOrIri = ql::exportIds::idToLiteralOrIri(
      *index_, getId(component), emptyLocalVocab);
  if (!literalOrIri.has_value()) {
    return {};
  }
  return std::string{
      std::move(literalOrIri).value().toStringRepresentation()};
}

// _____________________________________________________________________________
void ScanCursor::skip(uint64_t offset) {
  // NOTE: For a cursor over distinct values, `offset` counts groups, not stored
  // triples, so the block skipping of `firstBlockAfterOffset` (which counts
  // rows) does
  // not apply. Until the block metadata carries a count of distinct values, the
  // only correct way to reach the `offset`-th group is to walk the groups. See
  // the note on `skip` in the header.
  if (numDistinctComponents_ > 0) {
    if (offset < current_) {
      positionAt(0);
    }
    while (current_ < offset && hasNext()) {
      next();
    }
    current_ = std::max(current_, offset);
    return;
  }
  // Skipping before anything has been read costs nothing at all: the scan has
  // not been opened yet, so it will simply be opened at `offset`.
  if (!started_) {
    current_ = offset;
    return;
  }
  // Skipping forwards within the block that is already decompressed is free,
  // and it is the common case when a caller skips by a handful of elements.
  if (block_.has_value() && offset >= current_) {
    uint64_t delta = offset - current_;
    if (rowInBlock_ + delta < block_.value().numRows()) {
      rowInBlock_ += delta;
      current_ = offset;
      return;
    }
  }
  positionAt(offset);
}

// _____________________________________________________________________________
uint64_t ScanCursor::cardinality() const {
  if (numDistinctComponents_ > 0) {
    // NOTE: Counting the distinct values requires walking them, for the same
    // reason that `skip` does. `getDistinctColIdsAndCounts` would do it with
    // fewer decompressions, but it materializes all the values.
    ScanCursor counting{*index_,
                        boundIdPerComponent_.at(0),
                        boundIdPerComponent_.at(1),
                        boundIdPerComponent_.at(2),
                        locatedTriples_,
                        distinctOn_};
    while (counting.hasNext()) {
      counting.next();
    }
    return counting.current();
  }
  return index_->getPermutation(permutation_)
      .getResultSizeOfScan(scanSpecAndBlocks_.value(), *locatedTriples_);
}

// _____________________________________________________________________________
std::pair<uint64_t, uint64_t> ScanCursor::cardinalityEstimate() const {
  return index_->getPermutation(permutation_)
      .getSizeEstimateForScan(scanSpecAndBlocks_.value(), *locatedTriples_);
}

}  // namespace qlever
