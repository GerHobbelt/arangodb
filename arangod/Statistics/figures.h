////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Business Source License 1.1 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     https://github.com/arangodb/arangodb/blob/devel/LICENSE
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <atomic>
#include <mutex>
#include <vector>

namespace arangodb {
namespace statistics {

////////////////////////////////////////////////////////////////////////////////
/// @brief a simple counter
////////////////////////////////////////////////////////////////////////////////

struct Counter {
  constexpr Counter() noexcept : _count(0) {}

  Counter& operator=(Counter const& other) {
    _count.store(other._count.load());
    return *this;
  }

  void incCounter() noexcept { ++_count; }

  void decCounter() noexcept { --_count; }

  int64_t get() const noexcept {
    return _count.load(std::memory_order_relaxed);
  }

 private:
  std::atomic<int64_t> _count;
};

////////////////////////////////////////////////////////////////////////////////
/// @brief a distribution with count, min, max, mean, and variance
////////////////////////////////////////////////////////////////////////////////

struct Distribution {
  Distribution() : _count(0), _total(0.0), _cuts(), _counts() {}

  explicit Distribution(std::vector<double> const& dist)
      : _count(0), _total(0.0), _cuts(dist), _counts() {
    _counts.resize(_cuts.size() + 1);
  }

  Distribution& operator=(Distribution& other) {
    std::lock_guard l1{_mutex};
    std::lock_guard l2{other._mutex};

    _count = other._count;
    _total = other._total;
    _cuts = other._cuts;
    _counts = other._counts;

    return *this;
  }

  void addFigure(double value) {
    TRI_ASSERT(!_counts.empty());
    std::lock_guard lock{_mutex};

    ++_count;
    _total += value;

    std::vector<double>::iterator i = _cuts.begin();
    std::vector<uint64_t>::iterator j = _counts.begin();

    for (; i != _cuts.end(); ++i, ++j) {
      if (value < *i) {
        ++(*j);
        return;
      }
    }

    ++(*j);
  }

  void add(Distribution& other) {
    std::lock_guard lock{_mutex};
    std::lock_guard lock2{other._mutex};
    TRI_ASSERT(_counts.size() == other._counts.size() &&
               _cuts.size() == other._cuts.size());
    _count += other._count;
    _total += other._total;
    for (size_t i = 0; i < _counts.size(); ++i) {
      TRI_ASSERT(i < _cuts.size() ? _cuts[i] == other._cuts[i] : true);
      _counts[i] += other._counts[i];
    }
  }

  uint64_t _count;
  double _total;
  std::vector<double> _cuts;
  std::vector<uint64_t> _counts;

 private:
  std::mutex _mutex;
};
}  // namespace statistics
}  // namespace arangodb
