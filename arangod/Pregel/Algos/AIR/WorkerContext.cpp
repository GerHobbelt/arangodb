////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2020 ArangoDB GmbH, Cologne, Germany
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
/// @author Heiko Kernbach
/// @author Lars Maier
/// @author Markus Pfeiffer
////////////////////////////////////////////////////////////////////////////////

#include <Logger/LogMacros.h>
#include <Logger/Logger.h>

#include "WorkerContext.h"

namespace arangodb::pregel::algos::accumulators {

WorkerContext::WorkerContext(VertexAccumulators const* algorithm)
    : _algo(algorithm) {
  CustomAccumulatorDefinitions const& customDefinitions = _algo->options().customAccumulators;
  AccumulatorsDeclaration const& globalAccumulatorsDeclarations =
      _algo->options().globalAccumulators;

  for (auto&& acc : globalAccumulatorsDeclarations) {
    _globalAccumulators.emplace(acc.first, instantiateAccumulator(acc.second, customDefinitions));
    _globalAccumulatorsUpdates.emplace(acc.first, instantiateAccumulator(acc.second, customDefinitions));
  }
}

std::unordered_map<std::string, std::unique_ptr<AccumulatorBase>> const& WorkerContext::globalAccumulators() {
  return _globalAccumulators;
}

auto WorkerContext::globalAccumulatorsUpdates() -> std::unordered_map<std::string, MutexAccumPair> const&  {
  return _globalAccumulatorsUpdates;
}

void WorkerContext::preGlobalSuperstep(uint64_t gss) {}

void WorkerContext::preGlobalSuperstepMasterMessage(VPackSlice msg) {
  for (auto&& acc : globalAccumulatorsUpdates()) {
    auto res = acc.second.accum->clear();
    if (!res) {
      getReportManager().report(ReportLevel::ERR).with("accumulator", acc.first)
          << "error while clearing global accumulator update " << acc.first
          << " " + res.error().toString();
    }
  }

  auto globalAccumulatorValues = msg.get("globalAccumulatorValues");

  if (globalAccumulatorValues.isNull() || !globalAccumulatorValues.isObject()) {
    LOG_DEVEL << "worker did not receive valid global accumulator values, but "
              << globalAccumulatorValues.toJson();
    return;
  }

  for (auto&& upd : VPackObjectIterator(globalAccumulatorValues)) {
    if (!upd.key.isString()) {
      LOG_DEVEL << "global accumulator key is not a string, but " << upd.key.toJson();
      continue;
    }

    auto accumName = upd.key.copyString();

    if (auto iter = globalAccumulators().find(accumName);
        iter != std::end(globalAccumulators())) {
      auto res = iter->second->setStateBySlice(upd.value);
      if (!res) {
        getReportManager().report(ReportLevel::ERR).with("accumulator", accumName)
            << "worker could not set accumulator value for global accumulator "
            << accumName << " could not be set, " << res.error().toString();
      }
    }
  }
}

void WorkerContext::postGlobalSuperstep(uint64_t gss) {}

// Send the updates for the global accumulators back to the conductor
void WorkerContext::postGlobalSuperstepMasterMessage(VPackBuilder& msg) {
  {
    VPackObjectBuilder guard(&msg);
    {
      VPackObjectBuilder updateGuard(&msg, "globalAccumulatorUpdates");

      for (auto&& acc : globalAccumulatorsUpdates()) {
        msg.add(VPackValue(acc.first));
        auto res = acc.second.accum->getStateUpdateIntoBuilder(msg);
        if (!res) {
          getReportManager().report(ReportLevel::ERR).with("accumulator", acc.first)
              << "worker composing update for `" << acc.first
              << "` failed: " + res.error().toString();
        }
      }
    }
  }
}

greenspun::EvalResult WorkerContext::sendToGlobalAccumulator(std::string accumId,
                                                             VPackSlice msg) const {
  if (auto iter = _globalAccumulatorsUpdates.find(accumId);
      iter != std::end(_globalAccumulatorsUpdates)) {
    std::unique_lock guard(iter->second.mutex);
    return iter->second.accum->updateByMessageSlice(msg).asResult();
  }
  return greenspun::EvalError("global accumulator`" + accumId + "` not found");
}

greenspun::EvalResult WorkerContext::getGlobalAccumulator(std::string accumId,
                                                          VPackBuilder result) const {
  if (auto iter = _globalAccumulatorsUpdates.find(accumId);
      iter != std::end(_globalAccumulatorsUpdates)) {
    std::unique_lock guard(iter->second.mutex);
    return iter->second.accum->getIntoBuilder(result).asResult();
  }
  return greenspun::EvalError("global accumulator `" + accumId + "` not found");
}

}  // namespace arangodb::pregel::algos::accumulators
