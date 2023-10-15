////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2022-2022 ArangoDB GmbH, Cologne, Germany
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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#include "WaitForQueue.h"

#include "Basics/debugging.h"

using namespace arangodb;
using namespace arangodb::replication2;

auto WaitForQueue::waitFor(LogIndex index) -> WaitForQueue::WaitForFuture {
  auto it = _queue.emplace(index, WaitForPromise());

  return it->second.getFuture();
}

auto WaitForQueue::splitLowerThan(LogIndex commitIndex) noexcept
    -> WaitForQueue {
  auto toBeResolved = WaitForQueue();
  auto const end = _queue.upper_bound(commitIndex);
  for (auto it = _queue.begin(); it != end;) {
    toBeResolved._queue.insert(_queue.extract(it++));
  }
  return toBeResolved;
}

WaitForQueue::~WaitForQueue() {
  // TODO enable log message
  if (!_queue.empty()) {
    TRI_ASSERT(false) << "expected wait-for-queue to be empty";
    // LOG_CTX("ce7f7", ERR, _logContext)
    //     << "expected wait-for-queue to be empty";
  }
}
