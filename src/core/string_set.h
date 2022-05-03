// Copyright 2022, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//
#pragma once

#include "core/compact_object.h"

extern "C" {
#include "redis/object.h"
}

namespace dfly {

class StringSet {
  struct Entry {
    CompactObj value;
    uint8_t unused[6];

    Entry* next = nullptr;

    Entry() {
      Reset();
    }

    void Reset() {
      value.InitRobj(OBJ_LIST, 0, nullptr);  // OBJ_LIST serves as "empty" marker.
    }

    bool IsEmpty() const {
      return value.ObjType() == OBJ_LIST;
    }
  };

  static_assert(sizeof(Entry) == 32);

 public:
  class iterator;
  class const_iterator;
  using ItemCb = std::function<void(const CompactObj& co)>;

  StringSet(const StringSet&) = delete;

  explicit StringSet(std::pmr::memory_resource* mr = std::pmr::get_default_resource());
  ~StringSet();

  StringSet& operator=(StringSet&) = delete;

  void Reserve(size_t sz);

  bool Add(std::string_view str);

  bool Remove(std::string_view str);

  void Erase(iterator it);

  size_t size() const {
    return size_;
  }

  bool empty() const {
    return size_ == 0;
  }

  size_t bucket_count() const {
    return entries_.size();
  }

  // those that are chained to the entries stored inline in the bucket array.
  size_t num_chain_entries() const {
    return num_chain_entries_;
  }

  bool Contains(std::string_view val) const {
    uint64_t hc = CompactObj::HashCode(val);
    return FindAround(val, BucketId(hc)) < 2;
  }

  iterator begin() {
    return iterator{this, 0};
  }

  iterator end() {
    return iterator{};
  }

  /// stable scanning api. has the same guarantees as redis scan command.
  /// we avoid doing bit-reverse by using a different function to derive a bucket id
  /// from hash values. By using msb part of hash we make it "stable" with respect to
  /// rehashes. For example, with table log size 4 (size 16), entries in bucket id
  /// 1110 come from hashes 1110XXXXX.... When a table grows to log size 5,
  /// these entries can move either to 11100 or 11101. So if we traversed with our cursor
  /// range [0000-1110], it's guaranteed that in grown table we do not need to cover again
  /// [00000-11100]. Similarly with shrinkage, if a table is shrinked to log size 3,
  /// keys from 1110 and 1111 will move to bucket 111. Again, it's guaranteed that we
  /// covered the range [000-111] (all keys in that case).
  /// Returns: next cursor or 0 if reached the end of scan.
  /// cursor = 0 - initiates a new scan.
  uint32_t Scan(uint32_t cursor, const ItemCb& cb) const;

  unsigned BucketDepth(uint32_t bid) const;

  void IterateOverBucket(uint32_t bid, const ItemCb& cb);

  class iterator {
    friend class StringSet;

   public:
    iterator() : owner_(nullptr), entry_(nullptr), bucket_id_(0) {}

    iterator& operator++();

    bool operator==(const iterator& o) const {
      return entry_ == o.entry_;
    }

    bool operator!=(const iterator& o) const {
      return !(*this == o);
    }

    CompactObj& operator*() {
      return entry_->value;
    }

   private:
    iterator(StringSet* owner, uint32_t bid) : owner_(owner), bucket_id_(bid) {
      SeekNonEmpty();
    }

    void SeekNonEmpty();

    StringSet* owner_ = nullptr;
    Entry* entry_ = nullptr;
    uint32_t bucket_id_ = 0;
  };

  class const_iterator {
    friend class StringSet;

   public:
    const_iterator() : owner_(nullptr), entry_(nullptr), bucket_id_(0) {}

    const_iterator& operator++();

    const_iterator& operator=(iterator& it) {
      owner_ = it.owner_;
      entry_ = it.entry_;
      bucket_id_ = it.bucket_id_;

      return *this;
    }

    bool operator==(const const_iterator& o) const {
      return entry_ == o.entry_;
    }

    bool operator!=(const const_iterator& o) const {
      return !(*this == o);
    }

    const CompactObj& operator*() const {
      return entry_->value;
    }

   private:
    const_iterator(const StringSet* owner, uint32_t bid) : owner_(owner), bucket_id_(bid) {
      SeekNonEmpty();
    }

    void SeekNonEmpty();

    const StringSet* owner_ = nullptr;
    const Entry* entry_ = nullptr;
    uint32_t bucket_id_ = 0;
  };

 private:
  friend class iterator;

  using EntryAllocator = std::pmr::polymorphic_allocator<Entry>;
  using EntryAllocTraits = std::allocator_traits<EntryAllocator>;

  std::pmr::memory_resource* mr() {
    return entries_.get_allocator().resource();
  }

  static bool IsEmpty(const Entry& e) {
    return e.IsEmpty();
  }

  uint32_t BucketId(uint64_t hash) const {
    return hash >> (64 - capacity_log_);
  }

  // Returns: 2 if no empty spaces found around the bucket. 0, -1, 1 - offset towards
  // an empty bucket.
  int FindEmptyAround(uint32_t bid) const;

  // returns 2 if no object was found in the vicinity.
  // Returns relative offset to bid: 0, -1, 1 if found.
  int FindAround(std::string_view str, uint32_t bid) const;

  void Grow();
  void Link(CompactObj co, uint32_t bid);
  void MoveEntry(Entry* e, uint32_t bid);

  void Free(Entry* e) {
    mr()->deallocate(e, sizeof(Entry), alignof(Entry));
    --num_chain_entries_;
  }

  // The rule is - entries can be moved to vicinity as long as they are stored
  // "flat", i.e. not into the linked list. The linked list
  std::pmr::vector<Entry> entries_;
  uint32_t size_ = 0;
  uint32_t num_chain_entries_ = 0;
  unsigned capacity_log_ = 0;
};

inline StringSet::iterator& StringSet::iterator::operator++() {
  if (entry_->next) {
    entry_ = entry_->next;
  } else {
    ++bucket_id_;
    SeekNonEmpty();
  }

  return *this;
}

inline StringSet::const_iterator& StringSet::const_iterator::operator++() {
  if (entry_->next) {
    entry_ = entry_->next;
  } else {
    ++bucket_id_;
    SeekNonEmpty();
  }

  return *this;
}

}  // namespace dfly
