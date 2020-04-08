// Copyright 2018 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef NINJA_CONCURRENT_HASH_MAP_
#define NINJA_CONCURRENT_HASH_MAP_

#include <stdint.h>

#include <atomic>
#include <memory>
#include <thread>
#include <stdio.h>
#include <utility>
#include <vector>

#include "metrics.h"
#include "util.h"

/// A hash table that allows for concurrent lookups and insertions.
///
/// Resizing the table isn't thread-safe; inserting items doesn't automatically
/// resize the table.
template <typename K, typename V>
class ConcurrentHashMap {
public:
  using key_type = K;
  using mapped_type = V;
  using value_type = std::pair<const K, V>;
  using reference = value_type&;
  using pointer = value_type*;
  using const_reference = const value_type&;
  using const_pointer = const value_type*;

private:
  struct Node;

  struct NodeStub {
    std::atomic<Node*> next { nullptr };
  };

  struct Node : NodeStub {
    Node(size_t hash, const value_type& value) : hash(hash), value(value) {}

    size_t hash = 0;
    value_type value {};
  };

  /// Record the number of elements in the hash map. Assign each thread to a
  /// separate index within the size table to avoid false sharing among CPUs.
  struct NINJA_ALIGNAS_CACHE_LINE SizeRecord {
    std::atomic<ssize_t> size;
  };
  std::vector<SizeRecord> size_ { GetThreadSlotCount() };

  void IncrementSize() {
    thread_local size_t idx = GetThreadSlotIndex();
    size_[idx].size++;
  }

public:
  explicit ConcurrentHashMap(size_t size=1)
      : table_(std::max<size_t>(size, 1)) {}

  ConcurrentHashMap(const ConcurrentHashMap&) = delete;
  ConcurrentHashMap& operator=(const ConcurrentHashMap&) = delete;

  ~ConcurrentHashMap() {
    ForEachNode(table_, [](Node* node) {
      delete node;
    });
  }

  void swap(ConcurrentHashMap& other) {
    size_.swap(other.size_);
    table_.swap(other.table_);
  }

  /// Atomically lookup a key.
  V* Lookup(const K& key) {
    std::pair<NodeStub*, Node*> pos = Find(std::hash<K>()(key), key);
    return pos.first ? nullptr : &pos.second->value.second;
  }

  /// Atomically lookup a key.
  const V* Lookup(const K& key) const {
    auto self = const_cast<ConcurrentHashMap*>(this);
    std::pair<NodeStub*, Node*> pos = self->Find(std::hash<K>()(key), key);
    return pos.first ? nullptr : &pos.second->value.second;
  }

  /// Atomically insert a (K, V) entry into the hash map if K isn't already in
  /// the map. Does nothing if K is already in the map.
  ///
  /// Returns the address of the V entry in the map, and a bool denoting whether
  /// or not an insertion was performed.
  ///
  /// This function avoids returning an iterator because insertion is
  /// thread-safe while iteration isn't, so returning an iterator seemed
  /// hazardous.
  std::pair<V*, bool> insert(const value_type& value) {
    fprintf(stderr, "ins %lld\n", value);
    size_t hash = std::hash<K>()(value.first);
    fprintf(stderr, "insert %lld\n", hash);
    std::unique_ptr<Node> node(new Node(hash, value));
    std::pair<Node*, bool> result = InsertNode(std::move(node));
    return { &result.first->value.second, result.second };
  }

  size_t size() const {
    ssize_t result = 0;
    for (const SizeRecord& rec : size_)
      result += rec.size.load();
    return result;
  }
  bool empty() const { return size() == 0; }
  size_t bucket_count() const { return table_.size(); }

  /// Unlike insertion and lookup, rehashing is *not* thread-safe. This class
  /// does not automatically rehash the table on insertion.
  void rehash(size_t buckets) {
    METRIC_RECORD("ConcurrentHashMap rehash");

    std::vector<NodeStub> tmp_table(std::max<size_t>(buckets, 1));
    tmp_table.swap(table_);
    ForEachNode(tmp_table, [this](Node* node) {
      node->next = nullptr;
      InsertNode(std::unique_ptr<Node>(node));
    });
  }

  void reserve(size_t count) {
    if (count < bucket_count())
      return;
    rehash(count);
  }

private:
  template <typename Func>
  void ForEachNode(const std::vector<NodeStub>& table, Func&& func) {
    for (const NodeStub& stub : table) {
      Node* node = stub.next.load();
      while (node != nullptr) {
        Node* next = node->next.load();
        func(node);
        node = next;
      }
    }
  }

  /// Insert a node into the hash map if it isn't already there.
  std::pair<Node*, bool> InsertNode(std::unique_ptr<Node> node) {
    while (true) {
      std::pair<NodeStub*, Node*> pos = Find(node->hash, node->value.first);
      if (pos.first == nullptr)
        return { pos.second, false };

      node->next = pos.second;
      Node* expected = pos.second;
      if (pos.first->next.compare_exchange_weak(expected, node.get())) {
        IncrementSize();
        return { node.release(), true };
      }
    }
  }

  std::vector<NodeStub> table_;

  /// Find either a matching node or the insertion point for the node. To ensure
  /// deterministic behavior, the nodes are kept in ascending order, first by
  /// the hash value, then by the key itself.
  ///
  /// This function returns a pair of (stub, node):
  ///  - If the node is found, stub is null and node is non-null.
  ///  - If the node isn't found, stub is non-null, and node is the loaded value
  ///    of the stub, which should be used in the CAS to insert the node.
  std::pair<NodeStub*, Node*> Find(size_t hash, const K& key) {
    NodeStub* stub = &table_[hash % table_.size()];
    while (true) {
      Node* ptr = stub->next.load();
      if (ptr == nullptr || hash < ptr->hash) {
        return { stub, ptr };
      }
      if (hash == ptr->hash) {
        // In the common case, when the hash and ptr->hash are equal, we expect
        // the keys to be equal too, so compare for equality before comparing
        // for less-than.
        if (key == ptr->value.first) {
          return { nullptr, ptr };
        } else if (key < ptr->value.first) {
          return { stub, ptr };
        }
      }
      stub = ptr;
    }
  }

  struct MutCfg { using ValueType = value_type; };
  struct ConstCfg { using ValueType = const value_type; };

  /// Something like a ForwardIterator. Iteration over the table isn't
  /// thread-safe.
  template <typename Cfg>
  class iterator_impl {
    iterator_impl(const ConcurrentHashMap* container) : container_(container) {}
    iterator_impl(const ConcurrentHashMap* container, size_t bucket, Node* node)
        : container_(container),
          bucket_(bucket),
          node_(node) {}

    const ConcurrentHashMap* container_ = nullptr;
    size_t bucket_ = SIZE_MAX;
    Node* node_ = nullptr;

  public:
    /// This constructor is a copy constructor for const_iterator and an
    /// implicit conversion for const_iterator -> iterator.
    iterator_impl(const iterator_impl<ConstCfg>& other)
        : container_(other.container_),
          bucket_(other.bucket_),
          node_(other.node_) {}

    typename Cfg::ValueType& operator*() { return node_->value; }
    typename Cfg::ValueType* operator->() { return &node_->value; }

    iterator_impl<Cfg>& operator++() {
      node_ = node_->next.load();
      if (node_ == nullptr)
        *this = container_->FindNextOccupiedBucket(bucket_ + 1);
      return *this;
    }

    bool operator==(const iterator_impl<Cfg>& other) const {
      return node_ == other.node_;
    }
    bool operator!=(const iterator_impl<Cfg>& other) const {
      return node_ != other.node_;
    }

    friend class ConcurrentHashMap;
  };

public:
  using iterator = iterator_impl<MutCfg>;
  using const_iterator = iterator_impl<ConstCfg>;

  iterator begin() { return FindNextOccupiedBucket(0); }
  iterator end() { return iterator { this }; }
  const_iterator begin() const { return FindNextOccupiedBucket(0); }
  const_iterator end() const { return const_iterator { this }; }
  const_iterator cbegin() const { return FindNextOccupiedBucket(0); }
  const_iterator cend() const { return const_iterator { this }; }

private:
  const_iterator FindNextOccupiedBucket(size_t idx) const {
    while (idx < table_.size()) {
      Node* node = table_[idx].next.load();
      if (node != nullptr) {
        return const_iterator { this, idx, node };
      }
      ++idx;
    }
    return end();
  }
};

#endif  // NINJA_CONCURRENT_HASH_MAP_
