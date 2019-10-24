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
/// @author Markus Pfeiffer
////////////////////////////////////////////////////////////////////////////////

#include "ModificationExecutor.h"

#include "Aql/AllRowsFetcher.h"
#include "Aql/AqlValue.h"
#include "Aql/Collection.h"
#include "Aql/OutputAqlItemRow.h"
#include "Aql/SingleRowFetcher.h"
#include "Basics/Common.h"
#include "Basics/VelocyPackHelper.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Methods.h"
#include "VocBase/LogicalCollection.h"

#include "Aql/InsertModifier.h"
#include "Aql/RemoveModifier.h"
#include "Aql/SimpleModifier.h"
#include "Aql/UpdateReplaceModifier.h"
#include "Aql/UpsertModifier.h"

#include "Logger/LogMacros.h"

#include <algorithm>
#include "velocypack/velocypack-aliases.h"

using namespace arangodb;
using namespace arangodb::aql;
using namespace arangodb::basics;
namespace arangodb {
namespace aql {

ModifierOutput::ModifierOutput(InputAqlItemRow const inputRow, bool const error)
    : _inputRow(inputRow), _error(error), _oldValue(nullptr), _newValue(nullptr) {}

ModifierOutput::ModifierOutput(InputAqlItemRow const inputRow, bool const error,
                               std::unique_ptr<AqlValue>&& oldValue,
                               std::unique_ptr<AqlValue>&& newValue)
    : _inputRow(inputRow),
      _error(error),
      _oldValue(std::move(oldValue)),
      _newValue(std::move(newValue)) {}

InputAqlItemRow ModifierOutput::getInputRow() const { return _inputRow; }
bool ModifierOutput::isError() const { return _error; }
bool ModifierOutput::hasOldValue() const { return _oldValue != nullptr; }
AqlValue&& ModifierOutput::getOldValue() const { return std::move(*_oldValue); }
bool ModifierOutput::hasNewValue() const { return _newValue != nullptr; }
AqlValue&& ModifierOutput::getNewValue() const { return std::move(*_newValue); }

template <typename FetcherType, typename ModifierType>
ModificationExecutor<FetcherType, ModifierType>::ModificationExecutor(Fetcher& fetcher,
                                                                      Infos& infos)
    : _lastState(ExecutionState::HASMORE), _infos(infos), _fetcher(fetcher), _modifier(infos) {
  // In MMFiles we need to make sure that the data is not moved in memory or collected
  // for this collection as soon as we start writing to it.
  // This pin makes sure that no memory is moved pointers we get from a collection stay
  // correct until we release this pin
  _infos._trx->pinData(this->_infos._aqlCollection->id());

  // TODO: explain this abomination
  auto* trx = _infos._trx;
  TRI_ASSERT(trx != nullptr);
  bool const isDBServer = trx->state()->isDBServer();
  _infos._producesResults = ProducesResults(
      _infos._producesResults || (isDBServer && _infos._ignoreDocumentNotFound));
}

template <typename FetcherType, typename ModifierType>
ModificationExecutor<FetcherType, ModifierType>::~ModificationExecutor() = default;

// Fetches as many rows as possible from upstream using the fetcher's fetchRow
// method and accumulates results through the modifier
template <typename FetcherType, typename ModifierType>
std::pair<ExecutionState, typename ModificationExecutor<FetcherType, ModifierType>::Stats>
ModificationExecutor<FetcherType, ModifierType>::doCollect(size_t const maxOutputs) {
  // for fetchRow
  InputAqlItemRow row{CreateInvalidInputRowHint{}};
  ExecutionState state = ExecutionState::HASMORE;

  // Maximum number of rows we can put into output
  // So we only ever produce this many here
  // TODO: If we SKIP_IGNORE, then we'd be able to output more;
  //       this would require some counting to happen in the modifier
  while (_modifier.nrOfOperations() < maxOutputs && state != ExecutionState::DONE) {
    std::tie(state, row) = _fetcher.fetchRow(maxOutputs);
    if (state == ExecutionState::WAITING) {
      return {ExecutionState::WAITING, ModificationStats{}};
    }
    if (row.isInitialized()) {
      // Make sure we have a valid row
      TRI_ASSERT(row.isInitialized());
      _modifier.accumulate(row);
    }
  }
  TRI_ASSERT(state == ExecutionState::DONE || state == ExecutionState::HASMORE);
  return {state, ModificationStats{}};
}

// Outputs accumulated results, and counts the statistics
template <typename FetcherType, typename ModifierType>
void ModificationExecutor<FetcherType, ModifierType>::doOutput(OutputAqlItemRow& output,
                                                               Stats& stats) {
  _modifier.setupIterator();
  while (!_modifier.isFinishedIterator()) {
    ModifierOutput modifierOutput{_modifier.getOutput()};

    if (!modifierOutput.isError()) {
      if (_infos._options.returnOld) {
        output.cloneValueInto(_infos._outputOldRegisterId, modifierOutput.getInputRow(),
                              modifierOutput.getOldValue());
      }
      if (_infos._options.returnNew) {
        output.cloneValueInto(_infos._outputNewRegisterId, modifierOutput.getInputRow(),
                              modifierOutput.getNewValue());
      }
      if (!_infos._options.returnOld && !_infos._options.returnNew) {
        output.copyRow(modifierOutput.getInputRow());
      }
      // only advance row if we produced something
      output.advanceRow();
    }
    _modifier.advanceIterator();
  }

  if (_infos._doCount) {
    stats.addWritesExecuted(_modifier.nrOfWritesExecuted());
    stats.addWritesIgnored(_modifier.nrOfWritesIgnored());
  }
}

template <typename FetcherType, typename ModifierType>
std::pair<ExecutionState, typename ModificationExecutor<FetcherType, ModifierType>::Stats>
ModificationExecutor<FetcherType, ModifierType>::produceRows(OutputAqlItemRow& output) {
  TRI_ASSERT(_infos._trx);

  ModificationExecutor::Stats stats;

  const size_t maxOutputs = std::min(output.numRowsLeft(), _modifier.getBatchSize());

  // if we returned "WAITING" the last time we still have
  // documents in the accumulator that we have not submitted
  // yet
  if (_lastState != ExecutionState::WAITING) {
    _modifier.reset();
  }

  std::tie(_lastState, stats) = doCollect(maxOutputs);

  if (_lastState == ExecutionState::WAITING) {
    return {ExecutionState::WAITING, std::move(stats)};
  }

  TRI_ASSERT(_lastState == ExecutionState::DONE || _lastState == ExecutionState::HASMORE);

  _modifier.transact();

  // If the query is silent, there is no way to relate
  // the results slice contents and the submitted documents
  // If the query is *not* silent, we should get one result
  // for every document.
  // Yes. Really.
  TRI_ASSERT(_infos._options.silent || _modifier.nrOfDocuments() == _modifier.nrOfResults());

  doOutput(output, stats);

  return {_lastState, std::move(stats)};
}

using NoPassthroughSingleRowFetcher = SingleRowFetcher<BlockPassthrough::Disable>;

template class ::arangodb::aql::ModificationExecutor<NoPassthroughSingleRowFetcher, InsertModifier>;
template class ::arangodb::aql::ModificationExecutor<AllRowsFetcher, InsertModifier>;
template class ::arangodb::aql::ModificationExecutor<NoPassthroughSingleRowFetcher, RemoveModifier>;
template class ::arangodb::aql::ModificationExecutor<AllRowsFetcher, RemoveModifier>;
template class ::arangodb::aql::ModificationExecutor<NoPassthroughSingleRowFetcher, UpdateReplaceModifier>;
template class ::arangodb::aql::ModificationExecutor<AllRowsFetcher, UpdateReplaceModifier>;
template class ::arangodb::aql::ModificationExecutor<NoPassthroughSingleRowFetcher, UpsertModifier>;
template class ::arangodb::aql::ModificationExecutor<AllRowsFetcher, UpsertModifier>;

}  // namespace aql
}  // namespace arangodb
