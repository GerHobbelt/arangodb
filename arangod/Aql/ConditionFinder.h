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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Aql/ExecutionNode/ExecutionNode.h"
#include "Aql/ExecutionNodeId.h"
#include "Aql/WalkerWorker.h"

#include <cstdint>
#include <unordered_map>

namespace arangodb::aql {

class Condition;
class ExecutionPlan;
class SortCondition;
struct Variable;

/// @brief condition finder
class ConditionFinder final
    : public WalkerWorker<ExecutionNode, WalkerUniqueness::NonUnique> {
 public:
  ConditionFinder(ExecutionPlan* plan,
                  std::unordered_map<ExecutionNodeId, ExecutionNode*>& changes);

  ~ConditionFinder() = default;

  bool before(ExecutionNode*) override;

  bool enterSubquery(ExecutionNode*, ExecutionNode*) override;

  bool producesEmptyResult() const { return _producesEmptyResult; }

 protected:
  bool handleFilterCondition(ExecutionNode* en,
                             std::unique_ptr<Condition> const& condition,
                             bool& noRemoves);
  void handleSortCondition(ExecutionNode* en, Variable const* outVar,
                           std::unique_ptr<Condition> const& condition,
                           std::unique_ptr<SortCondition>& sortCondition);

 private:
  ExecutionPlan* _plan;
  std::unordered_map<VariableId, AstNode const*> _variableDefinitions;
  ::arangodb::containers::HashSet<VariableId> _filters;
  std::vector<std::pair<Variable const*, bool>> _sorts;
  // note: this class will never free the contents of this map
  std::unordered_map<aql::ExecutionNodeId, ExecutionNode*>& _changes;
  bool _producesEmptyResult;
};

}  // namespace arangodb::aql
