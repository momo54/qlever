// Copyright 2026 The QLever Authors
//
// You may not use this file except in compliance with the Apache 2.0 License,
// which can be found in the `LICENSE` file at the root of the QLever project.

#include "libqlever/ScanCursor.h"

#include "index/ExportIds.h"
#include "index/IndexImpl.h"

namespace qlever {

// _____________________________________________________________________________
Permutation::Enum ScanCursor::permutationFor(bool subjectIsBound,
                                             bool predicateIsBound,
                                             bool objectIsBound) {
  using enum Permutation::Enum;
  // The bound components have to be a prefix of the permutation, see
  // `ScanSpecification::validate`. Where several permutations qualify, the
  // choice is arbitrary but fixed, so that the order in which a triple pattern
  // is enumerated only depends on the pattern itself.
  if (subjectIsBound) {
    return objectIsBound && !predicateIsBound ? SOP : SPO;
  }
  if (predicateIsBound) {
    return objectIsBound ? POS : PSO;
  }
  return objectIsBound ? OSP : SPO;
}

// _____________________________________________________________________________
ScanCursor::ScanCursor(const IndexImpl& index,
                       std::optional<TripleComponent> subject,
                       std::optional<TripleComponent> predicate,
                       std::optional<TripleComponent> object,
                       LocatedTriplesSharedState locatedTriples)
    : index_{&index},
      locatedTriples_{std::move(locatedTriples)},
      permutation_{permutationFor(subject.has_value(), predicate.has_value(),
                                  object.has_value())},
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

  initialize(index, ScanSpecificationAsTripleComponent{cols[0], cols[1],
                                                       cols[2]}
                        .toScanSpecification(index));
}

// _____________________________________________________________________________
ScanCursor::ScanCursor(const IndexImpl& index, std::optional<Id> subject,
                       std::optional<Id> predicate, std::optional<Id> object,
                       LocatedTriplesSharedState locatedTriples)
    : index_{&index},
      locatedTriples_{std::move(locatedTriples)},
      permutation_{permutationFor(subject.has_value(), predicate.has_value(),
                                  object.has_value())},
      keyOrder_{Permutation::toKeyOrder(permutation_)},
      cancellationHandle_{
          std::make_shared<ad_utility::CancellationHandle<>>()} {
  std::array<std::optional<Id>, 3> spo{subject, predicate, object};
  std::array<std::optional<Id>, 3> cols;
  for (size_t i = 0; i < 3; ++i) {
    cols.at(i) = spo.at(keyOrder_.keys().at(i));
  }
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
  scanSpec_ = std::move(spec);
  scanSpecAndBlocks_ = index.getPermutation(permutation_)
                           .getScanSpecAndBlocks(scanSpec_.value(),
                                                 *locatedTriples_);
  restartAt(0);
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
void ScanCursor::restartAt(uint64_t offset) {
  current_ = offset;
  block_.reset();
  rowInBlock_ = 0;
  blocks_ = index_->getPermutation(permutation_)
                .lazyScan(scanSpecAndBlocks_.value(), std::nullopt, {},
                          cancellationHandle_, *locatedTriples_,
                          LimitOffsetClause{std::nullopt, offset});
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
bool ScanCursor::hasNext() const { return block_.has_value(); }

// _____________________________________________________________________________
void ScanCursor::next() {
  AD_CONTRACT_CHECK(hasNext());
  ++rowInBlock_;
  ++current_;
  advanceToNextNonEmptyBlock();
}

// _____________________________________________________________________________
Id ScanCursor::getId(Component component) const {
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
std::string ScanCursor::getString(Component component) const {
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
  restartAt(offset);
}

// _____________________________________________________________________________
uint64_t ScanCursor::cardinality() const {
  return index_->getPermutation(permutation_)
      .getResultSizeOfScan(scanSpecAndBlocks_.value(), *locatedTriples_);
}

// _____________________________________________________________________________
std::pair<uint64_t, uint64_t> ScanCursor::cardinalityEstimate() const {
  return index_->getPermutation(permutation_)
      .getSizeEstimateForScan(scanSpecAndBlocks_.value(), *locatedTriples_);
}

}  // namespace qlever
