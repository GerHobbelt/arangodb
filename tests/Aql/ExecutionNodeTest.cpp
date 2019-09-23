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
/// @author Markus Pfeiffer
////////////////////////////////////////////////////////////////////////////////


#include "gtest/gtest.h"

#include "Aql/Ast.h"
#include "Aql/Query.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/SubqueryStartExecutionNode.h"
#include "Aql/SubqueryEndExecutionNode.h"

#include "Logger/LogMacros.h"

#include "velocypack/Builder.h"
#include "velocypack/velocypack-aliases.h"

#include "../Mocks/Servers.h"

using namespace arangodb::aql;

namespace arangodb {
namespace tests {
namespace aql {

class ExecutionNodeTest : public ::testing::Test {
protected:
  mocks::MockAqlServer server;
  std::unique_ptr<arangodb::aql::Query> fakedQuery;
  Ast ast;
  ExecutionPlan plan;

public:
 ExecutionNodeTest()
     : fakedQuery(server.createFakeQuery()),
       ast(fakedQuery.get()),
       plan(&ast)
           {};
};

TEST_F(ExecutionNodeTest, start_node_velocypack_roundtrip) {
  VPackBuilder builder;

  std::unique_ptr<SubqueryStartNode> node, nodeFromVPack;

  node = std::make_unique<SubqueryStartNode>(&plan, 0);

  builder.openArray();
  node->toVelocyPackHelper(builder, ExecutionNode::SERIALIZE_DETAILS);
  builder.close();

  nodeFromVPack = std::make_unique<SubqueryStartNode>(&plan, builder.slice()[0]);

  ASSERT_TRUE(node->isEqualTo(*nodeFromVPack));
}

TEST_F(ExecutionNodeTest, start_node_not_equal_different_id) {
  std::unique_ptr<SubqueryStartNode> node1, node2;

  node1 = std::make_unique<SubqueryStartNode>(&plan, 0);
  node2 = std::make_unique<SubqueryStartNode>(&plan, 1);

  ASSERT_FALSE(node1->isEqualTo(*node2));
}

TEST_F(ExecutionNodeTest, end_node_velocypack_roundtrip) {
  VPackBuilder builder;

  Variable outvar("name", 1);

  std::unique_ptr<SubqueryEndNode> node, nodeFromVPack;

  node = std::make_unique<SubqueryEndNode>(&plan, 0, &outvar);

  builder.openArray();
  node->toVelocyPackHelper(builder, ExecutionNode::SERIALIZE_DETAILS);
  builder.close();

  nodeFromVPack = std::make_unique<SubqueryEndNode>(&plan, builder.slice()[0]);

  ASSERT_TRUE(node->isEqualTo(*nodeFromVPack));
}

TEST_F(ExecutionNodeTest, end_node_not_equal_different_id) {
  std::unique_ptr<SubqueryEndNode> node1, node2;

  Variable outvar("name", 1);

  node1 = std::make_unique<SubqueryEndNode>(&plan, 0, &outvar);
  node2 = std::make_unique<SubqueryEndNode>(&plan, 1, &outvar);

  ASSERT_FALSE(node1->isEqualTo(*node2));
}



} // namespace aql
} // namespace tests
} // namespace arangodb
