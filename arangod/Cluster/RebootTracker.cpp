////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2019 ArangoDB GmbH, Cologne, Germany
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

#include "RebootTracker.h"

#include "Scheduler/SchedulerFeature.h"
#include "lib/Basics/Exceptions.h"
#include "lib/Basics/ScopeGuard.h"
#include "lib/Logger/Logger.h"

#include <algorithm>

using namespace arangodb;
using namespace arangodb::cluster;

/*
 * TODO
 *  Think about how to handle failures gracefully. Mainly:
 *   - alloc failures
 *   - Scheduler->queue failures
 *  Maybe lazy deletion helps?
 */

RebootTracker::RebootTracker(RebootTracker::SchedulerPointer scheduler)
    : _scheduler(scheduler) {
  TRI_ASSERT(_scheduler != nullptr);
}

// TODO We should possibly get the complete list of peers from clusterinfo
//   (rather than only the list of changed peers) in order to be able to retry
//   regularly.
//   In this case, we must move the bidirectional comparison from
//   ClusterInfo.cpp here!

void RebootTracker::updateServerState(std::unordered_map<ServerID, RebootId> const& state) {
  MUTEX_LOCKER(guard, _mutex);

  // For all know servers, look whether they are changed or were removed
  for (auto curIt = _rebootIds.begin(); curIt != _rebootIds.end(); ++curIt) {
    auto const& serverId = curIt->first;
    auto& oldRebootId = curIt->second;
    auto const& newIt = state.find(serverId);

    if (newIt == state.end()) {
      // Try to schedule all callbacks for serverId.
      // If that didn't throw, erase the entry.
      scheduleAllCallbacksFor(serverId);
      auto it = _callbacks.find(serverId);
      if (it != _callbacks.end()) {
        TRI_ASSERT(it->second.empty());
        // TODO maybe do this in scheduleAllCallbacksFor? Maybe we can do it when we already have an iterator?
        _callbacks.erase(it);
      }
      _rebootIds.erase(curIt);
    } else {
      TRI_ASSERT(serverId == newIt->first);
      auto const& newRebootId = newIt->second;
      TRI_ASSERT(oldRebootId <= newRebootId);
      if (oldRebootId < newRebootId) {
        // Try to schedule all callbacks for serverId older than newRebootId.
        // If that didn't throw, erase the entry.
        scheduleCallbacksFor(serverId, newRebootId);
        oldRebootId = newRebootId;
      }
    }
  }

  // Look whether there are servers that are still unknown
  // (note: we could shortcut this and return if the sizes are equal, as at
  // this point, all entries in _rebootIds are also in state)
  for (auto const& newIt : state) {
    auto const& serverId = newIt.first;
    auto const& rebootId = newIt.second;
    auto rv = _rebootIds.emplace(serverId, rebootId);
    auto const inserted = rv.second;
    // If we inserted a new server, we may NOT already have any callbacks for
    // it!
    TRI_ASSERT(!inserted || _callbacks.find(serverId) == _callbacks.end());
  }
}

CallbackGuard RebootTracker::callMeOnChange(RebootTracker::PeerState const& peerState,
                                            RebootTracker::Callback callback,
                                            std::string callbackDescription) {
  MUTEX_LOCKER(guard, _mutex);

  // We MUST NOT insert something in _callbacks[serverId] unless _rebootIds[serverId] exists!
  if (_rebootIds.find(peerState.serverId()) == _rebootIds.end()) {
    std::string const error = [&]() {
      std::stringstream strstream;
      strstream << "When trying to register callback '" << callbackDescription << "': "
                << "The server " << peerState.serverId() << " is not known. "
                << "If this server joined the cluster in the last seconds, "
                   "this can happen.";
      return strstream.str();
    }();
    LOG_TOPIC("76abc", INFO, Logger::CLUSTER) << error;
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_CLUSTER_SERVER_UNKNOWN, error);
  }

  // For the given server, get the existing rebootId => [callbacks] map,
  // or create a new one
  auto& rebootIdMap = _callbacks[peerState.serverId()];
  // For the given rebootId, get the existing callbacks map,
  // or create a new one
  auto& callbackMapPtr = rebootIdMap[peerState.rebootId()];

  if (callbackMapPtr == nullptr) {
    // We must never leave a nullptr in here!
    // Try to create a new map, or remove the entry.
    try {
      callbackMapPtr =
          std::make_shared<std::remove_reference<decltype(callbackMapPtr)>::type::element_type>();
    } catch (...) {
      rebootIdMap.erase(peerState.rebootId());
      throw;
    }
  }

  TRI_ASSERT(callbackMapPtr != nullptr);

  auto& callbackMap = *callbackMapPtr;

  auto const callbackId = getNextCallbackId();

  // The guard constructor might, theoretically, throw. So we need to construct
  // it before emplacing the callback.
  auto callbackGuard =
      CallbackGuard([this, callbackId]() { unregisterCallback(callbackId); });

  auto emplaceRv =
      callbackMap.emplace(callbackId, DescriptedCallback{std::move(callback),
                                                         std::move(callbackDescription)});
  auto const iterator = emplaceRv.first;
  bool const inserted = emplaceRv.second;
  TRI_ASSERT(inserted);
  TRI_ASSERT(callbackId == iterator->first);

  // TODO I'm wondering why this compiles (with clang, at least), as the copy
  //      constructor is deleted. I don't think it should...
  return callbackGuard;
}

void RebootTracker::scheduleAllCallbacksFor(ServerID const& serverId) {
  scheduleCallbacksFor(serverId, RebootId::max());
  // Now the rebootId map of this server, if it exists, must be empty.
  TRI_ASSERT(_callbacks.find(serverId) == _callbacks.end() ||
             _callbacks.find(serverId)->second.empty());
}

// This function may throw.
// If (and only if) it returns, it has scheduled all affected callbacks, and
// removed them from the registry.
// Otherwise the state is unchanged.
void RebootTracker::scheduleCallbacksFor(ServerID const& serverId, RebootId rebootId) {
  _mutex.assertLockedByCurrentThread();

  auto serverIt = _callbacks.find(serverId);
  if (serverIt != _callbacks.end()) {
    auto& rebootMap = serverIt->second;
    auto const begin = rebootMap.begin();
    // lower_bounds returns the first iterator that is *not less than* rebootId
    auto const end = rebootMap.lower_bound(rebootId);

    std::vector<decltype(begin->second)> callbackSets;
    callbackSets.reserve(std::distance(begin, end));

    std::for_each(begin, end, [&callbackSets](auto it) {
      callbackSets.emplace_back(it.second);
    });

    // could throw
    queueCallbacks(std::move(callbackSets));

    // If and only if we successfully scheduled all callbacks, we erase them
    // from the registry.
    rebootMap.erase(begin, end);
  }
}

RebootTracker::Callback RebootTracker::createSchedulerCallback(
    std::vector<std::shared_ptr<std::unordered_map<CallbackId, DescriptedCallback>>> callbacks) {
  TRI_ASSERT(!callbacks.empty());
  TRI_ASSERT(std::none_of(callbacks.cbegin(), callbacks.cend(),
                          [](auto it) { return it == nullptr; }));
  TRI_ASSERT(std::none_of(callbacks.cbegin(), callbacks.cend(),
                          [](auto it) { return it->empty(); }));

  return [callbacks = std::move(callbacks)]() {
    TRI_ASSERT(!callbacks.empty());
    for (auto const& callbacksPtr : callbacks) {
      TRI_ASSERT(callbacksPtr != nullptr);
      TRI_ASSERT(!callbacksPtr->empty());
      for (auto const& it : *callbacksPtr) {
        auto const& cb = it.second.callback;
        auto const& descr = it.second.description;
        try {
          cb();
        } catch (arangodb::basics::Exception const& ex) {
          LOG_TOPIC("88a63", INFO, Logger::CLUSTER)
              << "Failed to execute reboot callback: " << descr << ": "
              << "[" << ex.code() << "] " << ex.what();
        } catch (std::exception const& ex) {
          LOG_TOPIC("88a63", INFO, Logger::CLUSTER)
              << "Failed to execute reboot callback: " << descr << ": " << ex.what();
        } catch (...) {
          LOG_TOPIC("88a63", INFO, Logger::CLUSTER)
              << "Failed to execute reboot callback: " << descr << ": "
              << "Unknown error.";
        }
      }
    }
  };
}

void RebootTracker::queueCallbacks(
    std::vector<std::shared_ptr<std::unordered_map<CallbackId, DescriptedCallback>>> callbacks) {
  if (callbacks.empty()) {
    return;
  }

  TRI_ASSERT(std::none_of(callbacks.cbegin(), callbacks.cend(),
                          [](auto it) { return it == nullptr; }));
  TRI_ASSERT(std::none_of(callbacks.cbegin(), callbacks.cend(),
                          [](auto it) { return it->empty(); }));

  auto cb = createSchedulerCallback(std::move(callbacks));

  // TODO which lane should we use?
  if (!_scheduler->queue(RequestLane::CLUSTER_INTERNAL, std::move(cb))) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_QUEUE_FULL,
        "No available threads when trying to queue cleanup "
        "callbacks due to a server reboot");
  }
}

// TODO Maybe we want to do this more efficiently, either by also passing
//      serverId and rebootId here in addition to callbackId, or an iterator.
//      Note that this happens once for every callback ever registered!
void RebootTracker::unregisterCallback(RebootTracker::CallbackId callbackId) {
  // Call cb for each iterator.
  auto for_each_iter = [](auto begin, auto end, auto cb) {
    auto it = begin;
    decltype(it) next;
    while (it != end) {
      // save next iterator now, in case cb invalidates it.
      next = std::next(it);
      cb(it);
      it = next;
    }
  };

  for_each_iter(_callbacks.begin(), _callbacks.end(), [&](auto const cbIt) {
    auto& rebootMap = cbIt->second;
    for_each_iter(rebootMap.cbegin(), rebootMap.cend(), [&](auto const rbIt) {
      auto& callbackSetPtr = rbIt->second;
      TRI_ASSERT(callbackSetPtr != nullptr);
      callbackSetPtr->erase(callbackId);
      if (callbackSetPtr->empty()) {
        rebootMap.erase(rbIt);
      }
    });
  });
}

RebootTracker::CallbackId RebootTracker::getNextCallbackId() noexcept {
  CallbackId nextId = _nextCallbackId;
  ++_nextCallbackId;
  return nextId;
}

CallbackGuard::CallbackGuard() : _callback(nullptr) {}

CallbackGuard::CallbackGuard(std::function<void(void)> callback)
    : _callback(std::move(callback)) {}

CallbackGuard::CallbackGuard(CallbackGuard&& other)
    : _callback(std::move(other._callback)) {
  other._callback = nullptr;
}

CallbackGuard& CallbackGuard::operator=(CallbackGuard&& other) {
  call();
  _callback = std::move(other._callback);
  other._callback = nullptr;
  return *this;
}

CallbackGuard::~CallbackGuard() { call(); }

void CallbackGuard::callAndClear() {
  call();
  _callback = nullptr;
}

void CallbackGuard::call() {
  if (_callback) {
    _callback();
  }
}
