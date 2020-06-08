/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#define DEBUG_TYPE "gc"

#include "hermes/VM/CardTableNC.h"

#include "hermes/Support/OSCompat.h"

#include <string.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>

namespace hermes {
namespace vm {

void CardTable::dirtyCardsForAddressRange(const void *low, const void *high) {
  dirtyRange(addressToIndex(low), addressToIndex(high));
}

OptValue<size_t> CardTable::findNextCardWithStatus(
    CardStatus status,
    size_t fromIndex,
    size_t endIndex) const {
  const char *fromIndexPtr = reinterpret_cast<const char *>(cards_) + fromIndex;
  const void *card =
      memchr(fromIndexPtr, static_cast<char>(status), endIndex - fromIndex);
  return (card == nullptr)
      ? OptValue<size_t>()
      : OptValue<size_t>(reinterpret_cast<const CardStatus *>(card) - cards_);
}

void CardTable::clear() {
  cleanRange(kFirstUsedIndex, kValidIndices - 1);
}

void CardTable::updateAfterCompaction(const void *newLevel) {
  static_assert(sizeof(CardTable) > 0, "CardTable cannot be of non-zero size");
  assert(
      newLevel >= base() + sizeof(CardTable) &&
      "New level must be strictly after the card table");

  const char *newLevelPtr = static_cast<const char *>(newLevel);

  // By assuming (see assertions above) that the card table is of non-zero size,
  // we can guarantee that it is always possible to subtract one from
  // newLevelPtr (which must be in the allocation region, and so after the card
  // table) and find an address that can be projected onto the card table.
  size_t lastDirtyCardIndex = addressToIndex(newLevelPtr - 1);

  // Dirty the occupied cards (below the level), and clean the cards above the
  // level.
  dirtyRange(kFirstUsedIndex, lastDirtyCardIndex);
  cleanRange(lastDirtyCardIndex + 1, kValidIndices - 1);
}

void CardTable::cleanRange(size_t from, size_t to) {
  cleanOrDirtyRange(from, to, CardStatus::Clean);
}

void CardTable::dirtyRange(size_t from, size_t to) {
  cleanOrDirtyRange(from, to, CardStatus::Dirty);
}

void CardTable::cleanOrDirtyRange(
    size_t from,
    size_t to,
    CardStatus cleanOrDirty) {
  for (size_t index = from; index <= to; index++) {
    cards_[index] = cleanOrDirty;
  }
}

void CardTable::updateBoundaries(
    CardTable::Boundary *boundary,
    const char *start,
    const char *end) {
  assert(boundary != nullptr && "Need a boundary cursor");
  assert(
      base() <= start && end <= AlignedStorage::end(base()) &&
      "Precondition: [start, end) must be covered by this table.");
  assert(
      boundary->index() == addressToIndex(boundary->address()) &&
      "Precondition: boundary's index must correspond to its address in this table.");
  // We must always have just crossed the boundary of the next card:
  assert(
      start <= boundary->address() && boundary->address() < end &&
      "Precondition: must have crossed boundary.");
  // The object may be large, and may cross multiple cards, but first
  // handle the first card.
  boundaries_[boundary->index()] =
      (boundary->address() - start) >> LogHeapAlign;
  boundary->bump();

  // Now we must fill in the remainder of the card boundaries crossed by the
  // allocation.  We use a logarithmic scheme, so we fill in one card
  // with -1, 2 with -2, etc., where each negative value k indicates
  // that we should go backwards by 2^(-k - 1) cards, and consult the
  // table there.
  int8_t currentExp = 0;
  int64_t currentIndexDelta = 1;
  int8_t numWithCurrentExp = 0;
  while (boundary->address() < end) {
    boundaries_[boundary->index()] = encodeExp(currentExp);
    numWithCurrentExp++;
    if (numWithCurrentExp == currentIndexDelta) {
      numWithCurrentExp = 0;
      currentExp++;
      currentIndexDelta *= 2;
      // Note that 7 bits handles object sizes up to 2^128, so we
      // don't have to worry about overflow of the int8_t.
    }
    boundary->bump();
  }
}

GCCell *CardTable::firstObjForCard(unsigned index) const {
  int8_t val = boundaries_[index];

  // If val is negative, it means skip backwards some number of cards.
  // In general, for an object crossing 2^N cards, a query for one of
  // those cards will examine at most N entries in the table.
  while (val < 0) {
    index -= 1 << decodeExp(val);
    val = boundaries_[index];
  }

  char *boundary = const_cast<char *>(indexToAddress(index));
  char *resPtr = boundary - (val << LogHeapAlign);
  return reinterpret_cast<GCCell *>(resPtr);
}

#ifdef HERMES_EXTRA_DEBUG
static void
protectBoundaryTableWork(void *table, size_t sz, oscompat::ProtectMode mode) {
  assert((reinterpret_cast<uintptr_t>(table) % oscompat::page_size()) == 0);
  assert((sz % oscompat::page_size()) == 0);
  bool res = oscompat::vm_protect(table, sz, mode);
  (void)res;
  assert(res);
}

void CardTable::protectBoundaryTable() {
  protectBoundaryTableWork(
      &boundaries_[0], kValidIndices, oscompat::ProtectMode::None);
}

void CardTable::unprotectBoundaryTable() {
  protectBoundaryTableWork(
      &boundaries_[0], kValidIndices, oscompat::ProtectMode::ReadWrite);
}
#endif // HERMES_EXTRA_DEBUG

#ifdef HERMES_SLOW_DEBUG
void CardTable::verifyBoundaries(char *start, char *level) const {
  // Start should be card-aligned.
  assert(isCardAligned(start));
  for (unsigned index = addressToIndex(start); index < kValidIndices; index++) {
    const char *boundary = indexToAddress(index);
    if (level <= boundary) {
      break;
    }
    GCCell *cell = firstObjForCard(index);
    // Should be a valid cell.
    assert(
        cell->isValid() &&
        "Card object boundary is broken: firstObjForCard yields invalid cell.");
    char *cellPtr = reinterpret_cast<char *>(cell);
    // And it should extend across the card boundary.
    assert(
        cellPtr <= boundary &&
        boundary < (cellPtr + cell->getAllocatedSize()) &&
        "Card object boundary is broken: first obj doesn't extend into card");
  }
}
#endif // HERMES_SLOW_DEBUG

} // namespace vm
} // namespace hermes