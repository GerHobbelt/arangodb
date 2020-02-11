////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "gtest/gtest.h"

#include "ExecutorTestHelper.h"
#include "RowFetcherHelper.h"

#include "Mocks/Servers.h"

#include "Aql/AqlItemBlock.h"
#include "Aql/ExecutionBlockImpl.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutorInfos.h"
#include "Aql/InputAqlItemRow.h"
#include "Aql/ResourceUsage.h"
#include "Aql/ReturnExecutor.h"
#include "Aql/SingleRowFetcher.h"

#include <velocypack/Builder.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::aql;

namespace arangodb {
namespace tests {
namespace aql {

// This is only to get a split-type. The Type is independent of actual template parameters
using ReturnExecutorTestHelper = ExecutorTestHelper<ReturnExecutor, 1, 1>;
using ReturnExecutorSplitType = ReturnExecutorTestHelper::SplitType;
using ReturnExecutorParamType = std::tuple<ReturnExecutorSplitType, bool>;

class ReturnExecutorTest : public ::testing::TestWithParam<ReturnExecutorParamType> {
 protected:
  // ExecutionState state;
  ResourceMonitor monitor{};
  mocks::MockAqlServer server{};
  AqlItemBlockManager itemBlockManager;

  std::unique_ptr<arangodb::aql::Query> fakedQuery;

  ReturnExecutorTest()
      : itemBlockManager(&monitor, SerializationFormat::SHADOWROWS),
        fakedQuery(server.createFakeQuery()) {
    auto engine =
        std::make_unique<ExecutionEngine>(*fakedQuery, SerializationFormat::SHADOWROWS);
    fakedQuery->setEngine(engine.release());
  }

  auto getSplit() -> ReturnExecutorSplitType {
    auto [split, unused] = GetParam();
    return split;
  }

  auto doCount() -> bool {
    auto [unused, doCount] = GetParam();
    return doCount;
  }

  auto getCountStats(size_t nr) -> ExecutionStats {
    ExecutionStats stats;
    if (doCount()) {
      stats.count = nr;
    }
    return stats;
  }
};

template <size_t... vs>
const ReturnExecutorSplitType splitIntoBlocks =
    ReturnExecutorSplitType{std::vector<std::size_t>{vs...}};
template <size_t step>
const ReturnExecutorSplitType splitStep = ReturnExecutorSplitType{step};

INSTANTIATE_TEST_CASE_P(ReturnExecutor, ReturnExecutorTest,
                        ::testing::Combine(::testing::Values(splitIntoBlocks<2, 3>,
                                                             splitIntoBlocks<3, 4>,
                                                             splitStep<1>, splitStep<2>),
                                           ::testing::Bool()));

/*******
 *  Start test suite
 ******/

/**
 * @brief Test the most basic query.
 *        We have an unlimited produce call
 *        And the data is in register 0 => we expect it to
 *        be passed through.
 */

TEST_P(ReturnExecutorTest, returns_all_from_upstream) {
  ReturnExecutorInfos infos(0 /*input register*/, 1 /*nr in*/, 1 /*nr out*/, doCount());
  AqlCall call{};  // unlimited produce
  ExecutorTestHelper<ReturnExecutor>(*fakedQuery)
      .setInputValueList(1, 2, 5, 2, 1, 5, 7, 1)
      .setInputSplitType(getSplit())
      .setCall(call)
      .expectOutput({0}, {{1}, {2}, {5}, {2}, {1}, {5}, {7}, {1}})
      .expectSkipped(0)
      .expectedState(ExecutionState::DONE)
      .expectedStats(getCountStats(8))
      .run(std::move(infos));
}

TEST_P(ReturnExecutorTest, handle_soft_limit) {
  ReturnExecutorInfos infos(0 /*input register*/, 1 /*nr in*/, 1 /*nr out*/, doCount());
  AqlCall call{};
  call.softLimit = 3;
  ExecutorTestHelper<ReturnExecutor>(*fakedQuery)
      .setInputValueList(1, 2, 5, 2, 1, 5, 7, 1)
      .setInputSplitType(getSplit())
      .setCall(call)
      .expectOutput({0}, {{1}, {2}, {5}})
      .expectSkipped(0)
      .expectedState(ExecutionState::HASMORE)
      .expectedStats(getCountStats(3))
      .run(std::move(infos));
}

TEST_P(ReturnExecutorTest, handle_hard_limit) {
  ReturnExecutorInfos infos(0 /*input register*/, 1 /*nr in*/, 1 /*nr out*/, doCount());
  AqlCall call{};
  call.hardLimit = 5;
  ExecutorTestHelper<ReturnExecutor>(*fakedQuery)
      .setInputValueList(1, 2, 5, 2, 1, 5, 7, 1)
      .setInputSplitType(getSplit())
      .setCall(call)
      .expectOutput({0}, {{1}, {2}, {5}, {2}, {1}})
      .expectSkipped(0)
      .expectedState(ExecutionState::DONE)
      .expectedStats(getCountStats(5))
      .run(std::move(infos));
}

TEST_P(ReturnExecutorTest, handle_offset) {
  ReturnExecutorInfos infos(0 /*input register*/, 1 /*nr in*/, 1 /*nr out*/, doCount());
  AqlCall call{};
  call.offset = 4;
  ExecutorTestHelper<ReturnExecutor>(*fakedQuery)
      .setInputValueList(1, 2, 5, 2, 1, 5, 7, 1)
      .setInputSplitType(getSplit())
      .setCall(call)
      .expectOutput({0}, {{1}, {5}, {7}, {1}})
      .expectSkipped(4)
      .expectedState(ExecutionState::DONE)
      .expectedStats(getCountStats(4))
      .run(std::move(infos));
}

TEST_P(ReturnExecutorTest, handle_fullcount) {
  ReturnExecutorInfos infos(0 /*input register*/, 1 /*nr in*/, 1 /*nr out*/, doCount());
  AqlCall call{};
  call.hardLimit = 2;
  call.fullCount = true;
  ExecutorTestHelper<ReturnExecutor>(*fakedQuery)
      .setInputValueList(1, 2, 5, 2, 1, 5, 7, 1)
      .setInputSplitType(getSplit())
      .setCall(call)
      .expectOutput({0}, {{1}, {2}})
      .expectSkipped(6)
      .expectedState(ExecutionState::DONE)
      .expectedStats(getCountStats(2))
      .run(std::move(infos));
}

TEST_P(ReturnExecutorTest, handle_other_inputRegister) {
  ReturnExecutorInfos infos(1 /*input register*/, 2 /*nr in*/, 1 /*nr out*/, doCount());
  AqlCall call{};
  call.hardLimit = 5;
  ExecutorTestHelper<ReturnExecutor, 2, 1>(*fakedQuery)
      .setInputValue({{R"("invalid")", 1},
                      {R"("invalid")", 2},
                      {R"("invalid")", 5},
                      {R"("invalid")", 2},
                      {R"("invalid")", 1},
                      {R"("invalid")", 5},
                      {R"("invalid")", 7},
                      {R"("invalid")", 1}})
      .setInputSplitType(getSplit())
      .setCall(call)
      .expectOutput({0}, {{1}, {2}, {5}, {2}, {1}})
      .expectSkipped(0)
      .expectedState(ExecutionState::DONE)
      .expectedStats(getCountStats(5))
      .run(std::move(infos));
}
}  // namespace aql
}  // namespace tests
}  // namespace arangodb
