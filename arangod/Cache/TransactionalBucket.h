////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Dan Larkin-York
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "Cache/BucketState.h"
#include "Cache/CachedValue.h"
#include "Cache/Common.h"

namespace arangodb::cache {

////////////////////////////////////////////////////////////////////////////////
/// @brief Bucket structure for TransactionalCache.
///
/// Contains a State variable, three slots each for hashes and data pointers,
/// four slots for banished hashes, and the applicable transaction term. Most
/// querying and manipulation can be handled via the exposed methods. Bucket
/// must be locked before doing anything else to ensure proper synchronization.
/// Data entries are carefully laid out to ensure the structure fits in a single
/// cacheline.
/// Note: the object used for hashing and comparison of values is not part of
/// every bucket, to save memory. Instead, the object used for hashing and
/// comparison is handed into the find() and remove() methods. It is required
/// that always the same hasher/comparator object is used for a given bucket.
////////////////////////////////////////////////////////////////////////////////
struct TransactionalBucket {
  BucketState _state;
  std::uint16_t _slotsUsed;

  // banish entries for transactional semantics
  static constexpr std::size_t kSlotsBanish = 5;
  std::uint32_t _banishHashes[kSlotsBanish];
  std::uint64_t _banishTerm;

  // actual cached entries
  static constexpr std::size_t kSlotsData = 8;
  std::uint32_t _cachedHashes[kSlotsData];
  CachedValue* _cachedData[kSlotsData];

  //////////////////////////////////////////////////////////////////////////////
  /// @brief Initialize an empty bucket.
  //////////////////////////////////////////////////////////////////////////////
  TransactionalBucket() noexcept;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief Attempt to lock bucket (failing after maxTries attempts).
  //////////////////////////////////////////////////////////////////////////////
  bool lock(std::uint64_t maxTries) noexcept;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief Unlock the bucket. Requires bucket to be locked.
  //////////////////////////////////////////////////////////////////////////////
  void unlock() noexcept;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief Checks whether the bucket is locked.
  //////////////////////////////////////////////////////////////////////////////
  bool isLocked() const noexcept;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief Checks whether the bucket has been migrated. Requires state to be
  /// locked.
  //////////////////////////////////////////////////////////////////////////////
  bool isMigrated() const noexcept;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief Checks whether bucket has been fully banished. Requires state to
  /// be locked.
  //////////////////////////////////////////////////////////////////////////////
  bool isFullyBanished() const noexcept;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief Checks whether bucket is full. Requires state to be locked.
  //////////////////////////////////////////////////////////////////////////////
  bool isFull() const noexcept;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief Looks up a given key and returns associated value. Requires state
  /// to be locked.
  ///
  /// Takes an input hash and key (specified by pointer and size), and searches
  /// the bucket for a matching entry. If a matching entry is found, it is
  /// returned. By default, a matching entry will be moved to the front of the
  /// bucket to allow basic LRU semantics. If no matching entry is found,
  /// nothing will be changed and a nullptr will be returned.
  //////////////////////////////////////////////////////////////////////////////
  template<typename Hasher>
  CachedValue* find(std::uint32_t hash, void const* key, std::size_t keySize,
                    bool moveToFront = true) noexcept;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief Inserts a given value if it is not banished. Requires state to
  /// be locked.
  ///
  /// Requires that the bucket is not full and does not already contain an item
  /// with the same key. If it is full, the item will not be inserted. If an
  /// item with the same key exists, this is not detected but it is likely to
  /// produce bugs later on down the line. If the item's hash has been
  /// banished, or the bucket is fully banished, insertion will simply do
  /// nothing. When inserting, the item is put into the first empty slot, then
  /// moved to the front. If attempting to insert and the bucket is full, the
  /// user should evict an item and specify the optimizeForInsertion flag to be
  /// true.
  //////////////////////////////////////////////////////////////////////////////
  void insert(std::uint32_t hash, CachedValue* value) noexcept;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief Removes an item with the given key if one exists. Requires state to
  /// be locked.
  ///
  /// Search for a matching key. If none exists, do nothing and return a
  /// nullptr. If one exists, remove it from the bucket and return the pointer
  /// to the value. Upon removal, the empty slot generated is moved to the back
  /// of the bucket (to remove the gap).
  //////////////////////////////////////////////////////////////////////////////
  template<typename Hasher>
  CachedValue* remove(std::uint32_t hash, void const* key,
                      std::size_t keySize) noexcept;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief Banishs a key and removes it if it exists. Requires state to
  /// be locked.
  ///
  /// Search for a matching key. If one exists, remove it. Then banish the
  /// hash associated with the key. If there are no empty banish slots, fully
  /// banish the bucket.
  //////////////////////////////////////////////////////////////////////////////
  template<typename Hasher>
  CachedValue* banish(std::uint32_t hash, void const* key,
                      std::size_t keySize) noexcept;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief Checks whether a given hash is banished. Requires state to be
  /// locked.
  //////////////////////////////////////////////////////////////////////////////
  bool isBanished(std::uint32_t hash) const noexcept;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief Searches for the best candidate in the bucket to evict. Requires
  /// state to be locked.
  ///
  /// Usually returns a pointer to least recently used freeable value. If the
  /// bucket contains no values or all have outstanding references, then it
  /// returns nullptr. In the case that ignoreRefCount is set to true, then it
  /// simply returns the least recently used value, regardless of freeability.
  //////////////////////////////////////////////////////////////////////////////
  CachedValue* evictionCandidate() const noexcept;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief evicts a candidate in the bucket. Requires state to be locked.
  /// Returns the size of the evicted value in case a value was evicted.
  /// Returns 0 otherwise.
  //////////////////////////////////////////////////////////////////////////////
  std::uint64_t evictCandidate() noexcept;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief Evicts the given value from the bucket. Requires state to be
  /// locked.
  ///
  /// By default, it will move the empty slot to the back of the bucket.
  //////////////////////////////////////////////////////////////////////////////
  void evict(CachedValue* value) noexcept;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief Updates the bucket's banish term. Requires state to be locked.
  //////////////////////////////////////////////////////////////////////////////
  void updateBanishTerm(std::uint64_t term) noexcept;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief Reinitializes a bucket to be completely empty and unlocked.
  /// Requires state to be locked.
  //////////////////////////////////////////////////////////////////////////////
  void clear() noexcept;

 private:
  /// @brief overrides the slot <slot> with the last populated slot, moving
  /// the contents of the last populated slot into <slot>. this is cheaper than
  /// closing the gap by moving all following slots one to the front.
  void closeGap(std::size_t slot) noexcept;

  void moveSlotToFront(std::size_t slot) noexcept;

  bool haveOpenTransaction() const noexcept;

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  void checkInvariants() const noexcept;
#else
  constexpr inline void checkInvariants() noexcept {}
#endif
};

// ensure that TransactionalBucket is exactly kBucketSizeInBytes
static_assert(sizeof(TransactionalBucket) == kBucketSizeInBytes,
              "Expected sizeof(TransactionalBucket) == kBucketSizeInBytes.");

}  // end namespace arangodb::cache
