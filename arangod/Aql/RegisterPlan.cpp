////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2019 ArangoDB GmbH, Cologne, Germany
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
/// @author Max Neunhoeffer
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "RegisterPlan.h"

#include "Aql/ClusterNodes.h"
#include "Aql/CollectNode.h"
#include "Aql/ExecutionNode.h"
#include "Aql/GraphNode.h"
#include "Aql/IResearchViewNode.h"
#include "Aql/IndexNode.h"
#include "Aql/ModificationNodes.h"
#include "Aql/SubqueryEndExecutionNode.h"

#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::aql;

// Requires RegisterPlan to be defined
VarInfo::VarInfo(unsigned int depth, RegisterId registerId)
    : depth(depth), registerId(registerId) {
  TRI_ASSERT(registerId < RegisterPlanT<ExecutionNode>::MaxRegisterId);
}

template <typename T>
void RegisterPlanWalkerT<T>::after(T* en) {
  TRI_ASSERT(en != nullptr);

  bool const isPassthrough = en->isPassthrough();
  if (!isPassthrough) {
    plan->increaseDepth();
  }

  if (en->getType() == ExecutionNode::SUBQUERY || en->getType() == ExecutionNode::SUBQUERY_END) {
    plan->addSubqueryNode(en);
  }

  /*
   * For passthrough blocks it is better to assign the registers _before_ we calculate
   * which registers have become unused to prevent reusing a input register as output register.
   *
   * This is not the case if the block is not passthrough since in that case the output row
   * is different from the input row.
   */
  auto const planRegistersForCurrentNode = [&](T* en, bool isBefore) -> void {
    if (isBefore == isPassthrough) {
      auto const outputVariables = en->getOutputVariables();
      for (VariableId const& v : outputVariables) {
        TRI_ASSERT(v != RegisterPlanT<T>::MaxRegisterId);
        plan->registerVariable(v, unusedRegisters);
      }
    }
  };

  auto const calculateRegistersToClear = [this](T* en) -> std::unordered_set<RegisterId> {
    ::arangodb::containers::HashSet<Variable const*> const& varsUsedLater =
        en->getVarsUsedLater();
    ::arangodb::containers::HashSet<Variable const*> varsUsedHere;
    en->getVariablesUsedHere(varsUsedHere);
    std::unordered_set<RegisterId> regsToClear;

    // Now find out which registers ought to be erased after this node:
    // ReturnNodes are special, since they return a single column anyway
    if (en->getType() != ExecutionNode::RETURN) {
      for (auto const& v : varsUsedHere) {
        auto it = varsUsedLater.find(v);

        if (it == varsUsedLater.end()) {
          auto it2 = plan->varInfo.find(v->id);

          if (it2 == plan->varInfo.end()) {
            // report an error here to prevent crashing
            THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                           std::string("missing variable #") +
                                               std::to_string(v->id) + " (" +
                                               v->name + ") for node #" +
                                               std::to_string(en->id().id()) +
                                               " (" + en->getTypeString() +
                                               ") while planning registers");
          }

          TRI_ASSERT(it2 != plan->varInfo.end());
          RegisterId r = it2->second.registerId;
          regsToClear.insert(r);
        }
      }
    }
    return regsToClear;
  };

  planRegistersForCurrentNode(en, true);
  auto regsToClear = calculateRegistersToClear(en);
  unusedRegisters.insert(regsToClear.begin(), regsToClear.end());
  // we can reuse all registers that belong to variables that are not in varsUsedLater and varsUsedHere
  planRegistersForCurrentNode(en, false);

  // We need to delete those variables that have been used here but are not
  // used any more later:
  en->setRegsToClear(std::move(regsToClear));

  en->_depth = plan->depth;
  en->_registerPlan = plan;
}

template <typename T>
RegisterPlanT<T>::RegisterPlanT() : depth(0), totalNrRegs(0) {
  nrRegs.reserve(8);
  nrRegs.emplace_back(0);
}

// Copy constructor used for a subquery:
template <typename T>
RegisterPlanT<T>::RegisterPlanT(RegisterPlan const& v, unsigned int newdepth)
    : varInfo(v.varInfo),
      nrRegs(v.nrRegs),
      subQueryNodes(),
      depth(newdepth + 1),
      totalNrRegs(v.nrRegs[newdepth]) {
  if (depth + 1 < 8) {
    // do a minium initial allocation to avoid frequent reallocations
    nrRegs.reserve(8);
  }
  // create a copy of the last value here
  // this is required because back returns a reference and emplace/push_back may
  // invalidate all references
  nrRegs.resize(depth);
  RegisterId registerId = nrRegs.back();
  nrRegs.emplace_back(registerId);
}

template <typename T>
RegisterPlanT<T>::RegisterPlanT(VPackSlice slice, unsigned int depth)
    : depth(depth),
      totalNrRegs(slice.get("totalNrRegs").getNumericValue<unsigned int>()) {
  VPackSlice varInfoList = slice.get("varInfoList");
  if (!varInfoList.isArray()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "\"varInfoList\" attribute needs to be an array");
  }

  varInfo.reserve(varInfoList.length());

  for (VPackSlice it : VPackArrayIterator(varInfoList)) {
    if (!it.isObject()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_NOT_IMPLEMENTED,
          "\"varInfoList\" item needs to be an object");
    }
    auto variableId = it.get("VariableId").getNumericValue<VariableId>();
    auto registerId = it.get("RegisterId").getNumericValue<RegisterId>();
    auto depthParam = it.get("depth").getNumericValue<unsigned int>();

    varInfo.try_emplace(variableId, VarInfo(depthParam, registerId));
  }

  VPackSlice nrRegsList = slice.get("nrRegs");
  if (!nrRegsList.isArray()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "\"nrRegs\" attribute needs to be an array");
  }

  nrRegs.reserve(nrRegsList.length());
  for (VPackSlice it : VPackArrayIterator(nrRegsList)) {
    nrRegs.emplace_back(it.getNumericValue<RegisterId>());
  }
}
template <typename T>
auto RegisterPlanT<T>::clone() -> std::shared_ptr<RegisterPlanT> {
  auto other = std::make_shared<RegisterPlanT>();

  other->nrRegs = nrRegs;
  other->depth = depth;
  other->totalNrRegs = totalNrRegs;
  other->varInfo = varInfo;

  // No need to clone subQueryNodes because this was only used during
  // the buildup.

  return other;
}

template <typename T>
void RegisterPlanT<T>::increaseDepth() {
  depth++;
  // create a copy of the last value here
  // this is required because back returns a reference and emplace/push_back
  // may invalidate all references
  RegisterId registerId = nrRegs.back();
  nrRegs.emplace_back(registerId);
}

template <typename T>
RegisterId RegisterPlanT<T>::addRegister() {
  nrRegs[depth]++;
  return totalNrRegs++;
}

template <typename T>
void RegisterPlanT<T>::registerVariable(VariableId v, std::set<RegisterId>& unusedRegisters) {
  RegisterId regId;

  if (unusedRegisters.empty()) {
    regId = addRegister();
  } else {
    auto iter = unusedRegisters.begin();
    regId = *iter;
    unusedRegisters.erase(iter);
  }

  bool inserted;
  std::tie(std::ignore, inserted) = varInfo.try_emplace(v, VarInfo(depth, regId));
  TRI_ASSERT(inserted);
  if (!inserted) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL,
        std::string("duplicate register assignment for variable #") +
            std::to_string(v) + " while planning registers");
  }
}

template <typename T>
void RegisterPlanT<T>::registerVariable(VariableId v) {
  auto regId = addRegister();

  varInfo.try_emplace(v, VarInfo(depth, regId));
}

template <typename T>
void RegisterPlanT<T>::toVelocyPackEmpty(VPackBuilder& builder) {
  builder.add(VPackValue("varInfoList"));
  { VPackArrayBuilder guard(&builder); }
  builder.add(VPackValue("nrRegs"));
  { VPackArrayBuilder guard(&builder); }
  // nrRegsHere is not used anymore and is intentionally left empty
  // can be removed in ArangoDB 3.8
  builder.add(VPackValue("nrRegsHere"));
  { VPackArrayBuilder guard(&builder); }
  builder.add("totalNrRegs", VPackValue(0));
}

template <typename T>
void RegisterPlanT<T>::toVelocyPack(VPackBuilder& builder) const {
  TRI_ASSERT(builder.isOpenObject());

  builder.add(VPackValue("varInfoList"));
  {
    VPackArrayBuilder guard(&builder);
    for (auto const& oneVarInfo : varInfo) {
      VPackObjectBuilder guardInner(&builder);
      builder.add("VariableId", VPackValue(oneVarInfo.first));
      builder.add("depth", VPackValue(oneVarInfo.second.depth));
      builder.add("RegisterId", VPackValue(oneVarInfo.second.registerId));
    }
  }

  builder.add(VPackValue("nrRegs"));
  {
    VPackArrayBuilder guard(&builder);
    for (auto const& oneRegisterID : nrRegs) {
      builder.add(VPackValue(oneRegisterID));
    }
  }

  // nrRegsHere is not used anymore and is intentionally left empty
  // can be removed in ArangoDB 3.8
  builder.add(VPackValue("nrRegsHere"));
  { VPackArrayBuilder guard(&builder); }

  builder.add("totalNrRegs", VPackValue(totalNrRegs));
}

template <typename T>
void RegisterPlanT<T>::addSubqueryNode(T* subquery) {
  subQueryNodes.emplace_back(subquery);
}

template <typename T>
auto RegisterPlanT<T>::getTotalNrRegs() -> unsigned int {
  return totalNrRegs;
}

template <typename T>
std::ostream& aql::operator<<(std::ostream& os, const RegisterPlanT<T>& r) {
  // level -> variable, info
  std::map<unsigned int, std::map<VariableId, VarInfo>> frames;

  for (auto [id, info] : r.varInfo) {
    frames[info.depth][id] = info;
  }

  for (auto [depth, vars] : frames) {
    os << "depth " << depth << std::endl;
    os << "------------------------------------" << std::endl;

    for (auto [id, info] : vars) {
      os << "id = " << id << " register = " << info.registerId << std::endl;
    }
  }
  return os;
}

template struct aql::RegisterPlanT<ExecutionNode>;
template struct aql::RegisterPlanWalkerT<ExecutionNode>;
template std::ostream& aql::operator<<<ExecutionNode>(std::ostream& os,
                                                      const RegisterPlanT<ExecutionNode>& r);
