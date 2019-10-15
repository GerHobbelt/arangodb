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

#ifndef ARANGOD_AQL_SIMPLE_MODIFIER_H
#define ARANGOD_AQL_SIMPLE_MODIFIER_H

#include "Aql/ModificationExecutor.h"
#include "Aql/ModificationExecutor2.h"
#include "Aql/ModificationExecutorTraits.h"

#include "Aql/InsertModifier.h"
#include "Aql/RemoveModifier.h"
#include "Aql/UpdateReplaceModifier.h"

#include <type_traits>

namespace arangodb {
namespace aql {

struct ModificationExecutorInfos;

//
// The SimpleModifier template class is the template for the simple modifiers
// Insert, Remove, Replace, and Update.
//
// It provides the accumulator for building up the VelocyPack that is submitted
// to the transaction, and a facility to iterate over the results of the
// operation.
//
// The only code that the ModifierCompletions have to implement is the
// accumulate and transact functions. The accumulate function collects the
// actual modifications and there is a specific one for Insert, Remove, and
// Update/Replace. The transact function calls the correct method for the
// transaction (insert, remove, update, replace), and the only difference
// between Update and Replace is which transaction method is called.
//

// Only classes that have is_modifier_completion_trait can be used as
// template parameter for SimpleModifier. This is mainly a safety measure
// to not run into ridiculous template errors
template <typename ModifierCompletion, typename _ = void>
struct is_modifier_completion_trait : std::false_type {};

template <>
struct is_modifier_completion_trait<InsertModifierCompletion> : std::true_type {};

template <>
struct is_modifier_completion_trait<RemoveModifierCompletion> : std::true_type {};

template <>
struct is_modifier_completion_trait<UpdateReplaceModifierCompletion> : std::true_type {
};

template <typename ModifierCompletion, typename Enable = typename std::enable_if_t<is_modifier_completion_trait<ModifierCompletion>::value>>
class SimpleModifier {
  friend class InsertModifierCompletion;
  friend class RemoveModifierCompletion;
  friend class UpdateReplaceModifierCompletion;

 public:
  using ModOp = std::pair<ModOperationType, InputAqlItemRow>;

 public:
  SimpleModifier(ModificationExecutorInfos& infos);
  ~SimpleModifier();

  void reset();
  void close();

  Result accumulate(InputAqlItemRow& row);
  Result transact();

  size_t nrOfOperations() const;

  // TODO: Rename
  size_t size() const;

  void throwTransactErrors();

  // TODO: Make this a real iterator
  Result setupIterator(ModifierIteratorMode const mode);
  bool isFinishedIterator();
  ModifierOutput getOutput();
  void advanceIterator();

  // We need to have a function that adds a document because returning a
  // (reference to a) slice has scoping problems
  void addDocument(VPackSlice const& doc);

  ModificationExecutorInfos& getInfos() const;

 private:
  ModificationExecutorInfos& _infos;
  ModifierCompletion _completion;

  std::vector<ModOp> _operations;
  VPackBuilder _accumulator;

  OperationResult _results;

  std::vector<ModOp>::const_iterator _operationsIterator;
  VPackArrayIterator _resultsIterator;
  ModifierIteratorMode _iteratorMode;
};

using InsertModifier = SimpleModifier<InsertModifierCompletion>;
using RemoveModifier = SimpleModifier<RemoveModifierCompletion>;
using UpdateReplaceModifier = SimpleModifier<UpdateReplaceModifierCompletion>;

}  // namespace aql
}  // namespace arangodb

#endif
