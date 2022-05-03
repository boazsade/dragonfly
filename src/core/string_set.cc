// Copyright 2022, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "core/string_set.h"

#include <absl/numeric/bits.h>

#include "base/logging.h"

namespace dfly {
using namespace std;

constexpr size_t kMinSizeShift = 2;
constexpr size_t kMinSize = 1 << kMinSizeShift;

StringSet::StringSet(pmr::memory_resource* mr) : entries_(mr) {
}

StringSet::~StringSet() {
  for (auto& entry : entries_) {
    DCHECK(entry.next == nullptr || !entry.IsEmpty());

    while (entry.next) {
      Entry* tmp = entry.next;
      entry.next = tmp->next;
      tmp->Reset();
      Free(tmp);
    }
  }
  DCHECK_EQ(0u, num_chain_entries_);
}

void StringSet::Reserve(size_t sz) {
  sz = std::min<size_t>(sz, kMinSize);

  sz = absl::bit_ceil(sz);
  capacity_log_ = absl::bit_width(sz);
  entries_.reserve(sz);
}

bool StringSet::Add(std::string_view str) {
  uint64_t hc = CompactObj::HashCode(str);

  if (entries_.empty()) {
    capacity_log_ = kMinSizeShift;
    entries_.resize(kMinSize);
    entries_[BucketId(hc)].value.SetString(str);

    return true;
  }

  uint32_t bucket_id = BucketId(hc);
  if (FindAround(str, bucket_id) < 2)
    return false;

  DCHECK_LT(bucket_id, entries_.size());
  ++size_;

  // Try insert into flat surface first. Also handle the grow case
  // if utilization is too high.
  for (unsigned j = 0; j < 2; ++j) {
    int offs = FindEmptyAround(bucket_id);
    if (offs < 2) {
      auto& entry = entries_[bucket_id + offs];
      entry.value.SetString(str);
      return true;
    }

    if (size_ < entries_.size())
      break;

    Grow();
    bucket_id = BucketId(hc);
  }

  // check displacement
  // TODO: we could avoid checking computing HC if we explicitly mark displacement.
  auto& dest = entries_[bucket_id];
  DCHECK(!IsEmpty(dest));
  uint64_t hc2 = dest.value.HashCode();
  uint32_t bid2 = BucketId(hc2);

  if (bid2 != bucket_id) {
    Link(std::move(dest.value), bid2);
    dest.value.SetString(str);
  } else {
    Link(CompactObj{str}, bucket_id);
  }
  return true;
}

unsigned StringSet::BucketDepth(uint32_t bid) const {
  const Entry& e = entries_[bid];
  if (e.IsEmpty()) {
    DCHECK(!e.next);
    return 0;
  }
  unsigned res = 1;
  const Entry* next = e.next;
  while (next) {
    ++res;
    next = next->next;
  }

  return res;
}

void StringSet::IterateOverBucket(uint32_t bid, const ItemCb& cb) {
  const Entry& e = entries_[bid];
  if (e.IsEmpty()) {
    DCHECK(!e.next);
    return;
  }
  cb(e.value);

  const Entry* next = e.next;
  while (next) {
    cb(next->value);
    next = next->next;
  }
}

int StringSet::FindAround(std::string_view str, uint32_t bid) const {
  const Entry* entry = entries_.data() + bid;
  do {
    if (entry->value == str) {
      return 0;
    }
    entry = entry->next;
  } while (entry);

  if (bid && entries_[bid - 1].value == str) {
    return -1;
  }

  if (bid + 1 < entries_.size() && entries_[bid + 1].value == str) {
    return 1;
  }

  return 2;
}

void StringSet::Grow() {
  size_t prev_sz = entries_.size();
  entries_.resize(prev_sz * 2);
  ++capacity_log_;

  for (int i = prev_sz - 1; i >= 0; --i) {
    Entry* root = entries_.data() + i;
    if (root->IsEmpty()) {
      DCHECK(!root->next);
      continue;
    }

    uint32_t bid = BucketId(root->value.HashCode());
    if (bid != uint32_t(i)) {
      // handle flat entry first.
      int offs = FindEmptyAround(bid);
      if (offs < 2) {
        auto& entry = entries_[bid + offs];
        DCHECK(!entry.next);
        entry.value = std::move(root->value);
      } else {
        Link(std::move(root->value), bid);
      }
      root->Reset();
    }

    Entry* next = root->next;
    Entry* prev = root;
    while (next) {
      uint32_t nbid = BucketId(next->value.HashCode());
      if (nbid != uint32_t(i)) {
        prev->next = next->next;
        MoveEntry(next, nbid);
      } else {
        prev = next;
      }
      next = prev->next;
    }

    next = root->next;
    if (root->IsEmpty() && next) {
      root->value = std::move(next->value);
      root->next = next->next;
      Free(next);
    }
  }
}

void StringSet::Link(CompactObj co, uint32_t bid) {
  Entry* dest = entries_.data() + bid;
  DCHECK(!IsEmpty(*dest));

  ++num_chain_entries_;

  EntryAllocator ea(mr());
  Entry* tail = ea.allocate(1);
  ea.construct(tail);

  // dest may hold object from a neighbour bucket id, hence we do not push further.
  tail->next = dest->next;
  dest->next = tail;
  tail->value = std::move(co);
}

void StringSet::MoveEntry(Entry* e, uint32_t bid) {
  auto& dest = entries_[bid];
  if (IsEmpty(dest)) {
    dest.value = std::move(e->value);
    Free(e);
    return;
  }
  e->next = dest.next;
  dest.next = e;
}

int StringSet::FindEmptyAround(uint32_t bid) const {
  if (IsEmpty(entries_[bid]))
    return 0;

  if (bid + 1 < entries_.size() && IsEmpty(entries_[bid + 1]))
    return 1;

  if (bid && IsEmpty(entries_[bid - 1]))
    return -1;

  return 2;
}

uint32_t StringSet::Scan(uint32_t cursor, const ItemCb& cb) const {
  if (capacity_log_ == 0)
    return 0;

  uint32_t bucket_id = cursor >> (32 - capacity_log_);
  const_iterator it(this, bucket_id);

  if (it.entry_ == nullptr)
    return 0;

  bucket_id = it.bucket_id_;  // non-empty bucket
  do {
    cb(*it);
    ++it;
  } while (it.bucket_id_ == bucket_id);

  if (it.entry_ == nullptr)
    return 0;

  if (it.bucket_id_ == bucket_id + 1) { // cover displacement case
    // TODO: we could avoid checking computing HC if we explicitly mark displacement.
    // we have plenty-metadata to do so.
    uint32_t bid = BucketId((*it).HashCode());
    if (bid == it.bucket_id_) {
      cb(*it);
      ++it;
    }
  }

  return it.entry_ ? it.bucket_id_ << (32 - capacity_log_) : 0;
}


void StringSet::iterator::SeekNonEmpty() {
  while (bucket_id_ < owner_->entries_.size()) {
    if (!owner_->entries_[bucket_id_].IsEmpty()) {
      entry_ = &owner_->entries_[bucket_id_];
      return;
    }
    ++bucket_id_;
  }
  entry_ = nullptr;
}

void StringSet::const_iterator::SeekNonEmpty() {
  while (bucket_id_ < owner_->entries_.size()) {
    if (!owner_->entries_[bucket_id_].IsEmpty()) {
      entry_ = &owner_->entries_[bucket_id_];
      return;
    }
    ++bucket_id_;
  }
  entry_ = nullptr;
}

}  // namespace dfly
