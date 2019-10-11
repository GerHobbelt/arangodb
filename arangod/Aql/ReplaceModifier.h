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

#ifndef ARANGOD_AQL_REPLACE_MODIFIER_H
#define ARANGOD_AQL_REPLACE_MODIFIER_H

#include "Aql/ModificationExecutor.h"
#include "Aql/ModificationExecutorTraits.h"
#include "Aql/SimpleModifier.h"

namespace arangodb {
namespace aql {

struct ModificationExecutorInfos;

class ReplaceModifierCompletion {
 public:
  ReplaceModifierCompletion(SimpleModifier<ReplaceModifierCompletion>& modifier);
  ~ReplaceModifierCompletion();

  ModOperationType accumulate(InputAqlItemRow& row);
  OperationResult transact();

 private:
  SimpleModifier<ReplaceModifierCompletion>& _modifier;
};

using ReplaceModifier = SimpleModifier<ReplaceModifierCompletion>;

}  // namespace aql
}  // namespace arangodb
#endif
