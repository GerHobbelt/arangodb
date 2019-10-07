////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_BENCHMARK_BENCHMARK_COUNTER_H
#define ARANGODB_BENCHMARK_BENCHMARK_COUNTER_H 1

#include "Basics/Common.h"
#include "Basics/Mutex.h"
#include "Basics/MutexLocker.h"

namespace arangodb {
namespace arangobench {

template <class T>
class BenchmarkCounter {
 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief create the counter
  //////////////////////////////////////////////////////////////////////////////

  BenchmarkCounter(T initialValue, T const maxValue)
      : _mutex(),
        _value(initialValue),
        _maxValue(maxValue),
        _incompleteFailures(0),
        _failures(0),
        _done(0) {}

  //////////////////////////////////////////////////////////////////////////////
  /// @brief destroy the counter
  //////////////////////////////////////////////////////////////////////////////

  ~BenchmarkCounter() = default;

 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief get the counter value
  //////////////////////////////////////////////////////////////////////////////

  T getValue() {
    MUTEX_LOCKER(mutexLocker, this->_mutex);
    return _value;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get the failures value
  //////////////////////////////////////////////////////////////////////////////

  size_t failures() {
    MUTEX_LOCKER(mutexLocker, this->_mutex);
    return _failures;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get the failures value
  //////////////////////////////////////////////////////////////////////////////

  size_t incompleteFailures() {
    MUTEX_LOCKER(mutexLocker, this->_mutex);
    return _incompleteFailures;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get the next x until the max is reached
  //////////////////////////////////////////////////////////////////////////////

  T next(const T value) {
    T realValue = value;

    if (value == 0) {
      realValue = 1;
    }

    MUTEX_LOCKER(mutexLocker, this->_mutex);

    T oldValue = _value;
    if (oldValue + realValue > _maxValue) {
      _value = _maxValue;
      return _maxValue - oldValue;
    }

    _value += realValue;
    return realValue;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief report x as done
  //////////////////////////////////////////////////////////////////////////////

  void done(const T value) {
    MUTEX_LOCKER(mutexLocker, this->_mutex);

    _done += value;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get how many done
  //////////////////////////////////////////////////////////////////////////////

  T getDone() {
    MUTEX_LOCKER(mutexLocker, this->_mutex);

    return _done;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief register a failure
  //////////////////////////////////////////////////////////////////////////////

  void incFailures(size_t const value) {
    MUTEX_LOCKER(mutexLocker, this->_mutex);
    _failures += value;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief register a failure
  //////////////////////////////////////////////////////////////////////////////

  void incIncompleteFailures(size_t const value) {
    MUTEX_LOCKER(mutexLocker, this->_mutex);
    _incompleteFailures += value;
  }

 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief mutex protecting the counter
  //////////////////////////////////////////////////////////////////////////////

  arangodb::Mutex _mutex;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the current value
  //////////////////////////////////////////////////////////////////////////////

  T _value;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the maximum value
  //////////////////////////////////////////////////////////////////////////////

  T const _maxValue;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the number of incomplete replies
  //////////////////////////////////////////////////////////////////////////////

  size_t _incompleteFailures;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the number of errors
  //////////////////////////////////////////////////////////////////////////////

  size_t _failures;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the number of operations done
  //////////////////////////////////////////////////////////////////////////////

  T _done;
};
}  // namespace arangobench
}  // namespace arangodb

#endif
