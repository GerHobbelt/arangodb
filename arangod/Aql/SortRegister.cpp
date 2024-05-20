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
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#include "SortRegister.h"

#include "Aql/RegisterPlan.h"
#include "Aql/Variable.h"

namespace arangodb::aql {

SortRegister::SortRegister(RegisterId reg, SortElement const& element) noexcept
    : attributePath(element.attributePath), reg(reg), asc(element.ascending) {}

void SortRegister::fill(ExecutionPlan const& /*execPlan*/,
                        RegisterPlan const& regPlan,
                        std::vector<SortElement> const& elements,
                        std::vector<SortRegister>& sortRegisters) {
  sortRegisters.reserve(elements.size());
  auto const& vars = regPlan.varInfo;

  for (auto const& p : elements) {
    auto const varId = p.var->id;
    auto const it = vars.find(varId);
    TRI_ASSERT(it != vars.end());
    TRI_ASSERT(it->second.registerId.isValid());
    sortRegisters.emplace_back(it->second.registerId, p);
  }
}

}  // namespace arangodb::aql
