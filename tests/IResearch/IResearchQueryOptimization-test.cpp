////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2017 ArangoDB GmbH, Cologne, Germany
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
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#include "common.h"
#include "gtest/gtest.h"

#include "../Mocks/StorageEngineMock.h"

#if USE_ENTERPRISE
#include "Enterprise/Ldap/LdapFeature.h"
#endif

#include "3rdParty/iresearch/tests/tests_config.hpp"
#include "Aql/AqlFunctionFeature.h"
#include "Aql/AqlItemBlockSerializationFormat.h"
#include "Aql/Ast.h"
#include "Aql/ExecutionNode.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/ExpressionContext.h"
#include "Aql/OptimizerRulesFeature.h"
#include "Aql/Query.h"
#include "Aql/QueryRegistry.h"
#include "Basics/SmallVector.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/ClusterFeature.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "IResearch/IResearchAnalyzerFeature.h"
#include "IResearch/IResearchCommon.h"
#include "IResearch/IResearchFeature.h"
#include "IResearch/IResearchFilterFactory.h"
#include "IResearch/IResearchLink.h"
#include "IResearch/IResearchLinkHelper.h"
#include "IResearch/IResearchView.h"
#include "Logger/LogTopic.h"
#include "Logger/Logger.h"
#include "RestServer/AqlFeature.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/DatabasePathFeature.h"
#include "RestServer/FlushFeature.h"
#include "RestServer/QueryRegistryFeature.h"
#include "RestServer/SystemDatabaseFeature.h"
#include "RestServer/TraverserEngineRegistryFeature.h"
#include "RestServer/ViewTypesFeature.h"
#include "Sharding/ShardingFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "Transaction/Methods.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/OperationOptions.h"
#include "V8/v8-globals.h"
#include "V8Server/V8DealerFeature.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/LogicalView.h"
#include "VocBase/ManagedDocumentResult.h"

#include "IResearch/VelocyPackHelper.h"
#include "analysis/analyzers.hpp"
#include "analysis/token_attributes.hpp"
#include "search/boolean_filter.hpp"
#include "search/range_filter.hpp"
#include "search/term_filter.hpp"
#include "utils/utf8_path.hpp"

#include <velocypack/Iterator.h>

extern const char* ARGV0;  // defined in main.cpp

NS_LOCAL
static const VPackBuilder systemDatabaseBuilder = dbArgsBuilder();
static const VPackSlice   systemDatabaseArgs = systemDatabaseBuilder.slice();

bool findEmptyNodes(TRI_vocbase_t& vocbase, std::string const& queryString,
                    std::shared_ptr<arangodb::velocypack::Builder> bindVars = nullptr) {
  auto options = VPackParser::fromJson(
      //    "{ \"tracing\" : 1 }"
      "{ }");

  arangodb::aql::Query query(false, vocbase, arangodb::aql::QueryString(queryString),
                             bindVars, options, arangodb::aql::PART_MAIN);

  query.prepare(arangodb::QueryRegistryFeature::registry(), arangodb::aql::SerializationFormat::SHADOWROWS);

  arangodb::SmallVector<arangodb::aql::ExecutionNode*>::allocator_type::arena_type a;
  arangodb::SmallVector<arangodb::aql::ExecutionNode*> nodes{a};

  // try to find `EnumerateViewNode`s and process corresponding filters and sorts
  query.plan()->findNodesOfType(nodes, arangodb::aql::ExecutionNode::NORESULTS, true);
  return !nodes.empty();
}

// -----------------------------------------------------------------------------
// --SECTION--                                                 setup / tear-down
// -----------------------------------------------------------------------------

class IResearchQueryOptimizationTest : public ::testing::Test {
 protected:
  StorageEngineMock engine;
  arangodb::application_features::ApplicationServer server;
  std::vector<std::pair<arangodb::application_features::ApplicationFeature*, bool>> features;
  TRI_vocbase_t* _vocbase;
  std::deque<arangodb::ManagedDocumentResult> insertedDocs;

  IResearchQueryOptimizationTest()
      : engine(server), server(nullptr, nullptr), _vocbase(nullptr) {
    arangodb::EngineSelectorFeature::ENGINE = &engine;
    arangodb::aql::AqlFunctionFeature* functions = nullptr;

    arangodb::tests::init(true);

    // suppress INFO {authentication} Authentication is turned on (system only), authentication for unix sockets is turned on
    arangodb::LogTopic::setLogLevel(arangodb::Logger::AUTHENTICATION.name(),
                                    arangodb::LogLevel::WARN);

    // suppress log messages since tests check error conditions
    arangodb::LogTopic::setLogLevel(arangodb::Logger::FIXME.name(), arangodb::LogLevel::ERR);  // suppress WARNING DefaultCustomTypeHandler called
    arangodb::LogTopic::setLogLevel(arangodb::iresearch::TOPIC.name(),
                                    arangodb::LogLevel::FATAL);
    irs::logger::output_le(iresearch::logger::IRL_FATAL, stderr);

    // setup required application features
    features.emplace_back(new arangodb::FlushFeature(server), false);
    features.emplace_back(new arangodb::V8DealerFeature(server),
                          false);  // required for DatabaseFeature::createDatabase(...)
    features.emplace_back(new arangodb::ViewTypesFeature(server), true);
    features.emplace_back(new arangodb::AuthenticationFeature(server), true);
    features.emplace_back(new arangodb::DatabasePathFeature(server), false);
    features.emplace_back(new arangodb::DatabaseFeature(server), false);
    features.emplace_back(new arangodb::ShardingFeature(server), false);
    features.emplace_back(new arangodb::QueryRegistryFeature(server), false);  // must be first
    arangodb::application_features::ApplicationServer::server->addFeature(
        features.back().first);  // need QueryRegistryFeature feature to be added now in order to create the system database
    features.emplace_back(new arangodb::SystemDatabaseFeature(server), true);  // required for IResearchAnalyzerFeature
    features.emplace_back(new arangodb::TraverserEngineRegistryFeature(server), false);  // must be before AqlFeature
    features.emplace_back(new arangodb::AqlFeature(server), true);
    features.emplace_back(new arangodb::aql::OptimizerRulesFeature(server), true);
    features.emplace_back(functions = new arangodb::aql::AqlFunctionFeature(server),
                          true);  // required for IResearchAnalyzerFeature
    features.emplace_back(new arangodb::iresearch::IResearchAnalyzerFeature(server), true);
    features.emplace_back(new arangodb::iresearch::IResearchFeature(server), true);

#if USE_ENTERPRISE
    features.emplace_back(new arangodb::LdapFeature(server),
                          false);  // required for AuthenticationFeature with USE_ENTERPRISE
#endif

    // required for V8DealerFeature::prepare(), ClusterFeature::prepare() not required
    arangodb::application_features::ApplicationServer::server->addFeature(
        new arangodb::ClusterFeature(server));

    for (auto& f : features) {
      arangodb::application_features::ApplicationServer::server->addFeature(f.first);
    }

    for (auto& f : features) {
      f.first->prepare();
    }

    auto databases = VPackBuilder();
    databases.openArray();
    databases.add(systemDatabaseArgs);
    databases.close();

    auto* dbFeature =
        arangodb::application_features::ApplicationServer::lookupFeature<arangodb::DatabaseFeature>(
            "Database");
    dbFeature->loadDatabases(databases.slice());

    for (auto& f : features) {
      if (f.second) {
        f.first->start();
      }
    }

    // register fake non-deterministic function in order to suppress optimizations
    functions->add(arangodb::aql::Function{
        "_NONDETERM_", ".",
        arangodb::aql::Function::makeFlags(
            // fake non-deterministic
            arangodb::aql::Function::Flags::CanRunOnDBServer),
        [](arangodb::aql::ExpressionContext*, arangodb::transaction::Methods*,
           arangodb::aql::VPackFunctionParameters const& params) {
          TRI_ASSERT(!params.empty());
          return params[0];
        }});

    // register fake non-deterministic function in order to suppress optimizations
    functions->add(arangodb::aql::Function{
        "_FORWARD_", ".",
        arangodb::aql::Function::makeFlags(
            // fake deterministic
            arangodb::aql::Function::Flags::Deterministic, arangodb::aql::Function::Flags::Cacheable,
            arangodb::aql::Function::Flags::CanRunOnDBServer),
        [](arangodb::aql::ExpressionContext*, arangodb::transaction::Methods*,
           arangodb::aql::VPackFunctionParameters const& params) {
          TRI_ASSERT(!params.empty());
          return params[0];
        }});

    auto* analyzers =
        arangodb::application_features::ApplicationServer::lookupFeature<arangodb::iresearch::IResearchAnalyzerFeature>();
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    dbFeature->createDatabase(testDBInfo(), _vocbase);  // required for IResearchAnalyzerFeature::emplace(...)
    analyzers->emplace(result, "testVocbase::test_analyzer", "TestAnalyzer",
                       VPackParser::fromJson("\"abc\"")->slice());  // cache analyzer
    analyzers->emplace(result, "testVocbase::test_csv_analyzer",
                       "TestDelimAnalyzer",
                       VPackParser::fromJson("\",\"")->slice());  // cache analyzer

    auto* dbPathFeature =
        arangodb::application_features::ApplicationServer::getFeature<arangodb::DatabasePathFeature>(
            "DatabasePath");
    arangodb::tests::setDatabasePath(*dbPathFeature);  // ensure test data is stored in a unique directory
  }

  ~IResearchQueryOptimizationTest() {
    arangodb::AqlFeature(server).stop();  // unset singleton instance
    arangodb::LogTopic::setLogLevel(arangodb::iresearch::TOPIC.name(),
                                    arangodb::LogLevel::DEFAULT);
    arangodb::LogTopic::setLogLevel(arangodb::Logger::FIXME.name(),
                                    arangodb::LogLevel::DEFAULT);
    arangodb::application_features::ApplicationServer::server = nullptr;

    // destroy application features
    for (auto& f : features) {
      if (f.second) {
        f.first->stop();
      }
    }

    for (auto& f : features) {
      f.first->unprepare();
    }

    arangodb::LogTopic::setLogLevel(arangodb::Logger::AUTHENTICATION.name(),
                                    arangodb::LogLevel::DEFAULT);
    arangodb::EngineSelectorFeature::ENGINE = nullptr;
  }

void addLinkToCollection(std::shared_ptr<arangodb::iresearch::IResearchView>& view) {
  auto updateJson = VPackParser::fromJson(
    "{ \"links\" : {"
    "\"collection_1\" : { \"includeAllFields\" : true }"
    "}}");
  EXPECT_TRUE((view->properties(updateJson->slice(), true).ok()));

  arangodb::velocypack::Builder builder;

  builder.openObject();
  view->properties(builder, arangodb::LogicalDataSource::makeFlags(
    arangodb::LogicalDataSource::Serialize::Detailed));
  builder.close();

  auto slice = builder.slice();
  EXPECT_TRUE(slice.isObject());
  EXPECT_TRUE(slice.get("name").copyString() == "testView");
  EXPECT_TRUE(slice.get("type").copyString() ==
              arangodb::iresearch::DATA_SOURCE_TYPE.name());
  EXPECT_TRUE(slice.get("deleted").isNone());  // no system properties
  auto tmpSlice = slice.get("links");
  EXPECT_TRUE((true == tmpSlice.isObject() && 1 == tmpSlice.length()));
}

  TRI_vocbase_t& vocbase() {
    TRI_ASSERT(_vocbase != nullptr);
    return *_vocbase;
  }

  void SetUp() override {
  auto createJson = VPackParser::fromJson(
      "{ \
    \"name\": \"testView\", \
    \"type\": \"arangosearch\" \
  }");

  std::shared_ptr<arangodb::LogicalCollection> logicalCollection1;
  std::shared_ptr<arangodb::LogicalCollection> logicalCollection2;

  // add collection_1
  {
      auto collectionJson =
          VPackParser::fromJson("{ \"name\": \"collection_1\" }");
      logicalCollection1 = vocbase().createCollection(collectionJson->slice());
    ASSERT_TRUE((nullptr != logicalCollection1));
  }

  // add view
  auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
        vocbase().createView(createJson->slice()));
  ASSERT_TRUE((false == !view));

  // add link to collection
  addLinkToCollection(view);

  // populate view with the data
  {
    arangodb::OperationOptions opt;
      static std::vector<std::string> const EMPTY;
      arangodb::transaction::Methods trx(
          arangodb::transaction::StandaloneContext::Create(vocbase()), EMPTY,
          EMPTY, EMPTY, arangodb::transaction::Options());
    EXPECT_TRUE((trx.begin().ok()));

    // insert into collection
      auto builder =
          VPackParser::fromJson("[{ \"values\" : [ \"A\", \"C\", \"B\" ] }]");

    auto root = builder->slice();
    ASSERT_TRUE(root.isArray());

    for (auto doc : arangodb::velocypack::ArrayIterator(root)) {
      insertedDocs.emplace_back();
      auto const res =
          logicalCollection1->insert(&trx, doc, insertedDocs.back(), opt, false);
      EXPECT_TRUE(res.ok());
    }

    EXPECT_TRUE((trx.commit().ok()));
    EXPECT_TRUE((arangodb::iresearch::IResearchLinkHelper::find(*logicalCollection1, *view)
                     ->commit()
                     .ok()));
  }
  }
};  // IResearchQuerySetup

NS_END

// -----------------------------------------------------------------------------
// --SECTION--                                                        test suite
// -----------------------------------------------------------------------------

// dedicated to https://github.com/arangodb/arangodb/issues/8294
  // a IN [ x ] && a == y, x < y
TEST_F(IResearchQueryOptimizationTest, test_1) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ '@', 'A' ] AND d.values == 'C' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
      }
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a == y, x == y
TEST_F(IResearchQueryOptimizationTest, test_2) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'C', 'B', 'A' ] AND d.values "
        "== 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // FIXME
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
      }
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    }
    //{
    //  irs::Or expected;
    //  auto& root = expected.add<irs::And>();
    //  {
    //    auto& sub = root.add<irs::Or>();
    //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
    //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    //  }
    //  root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    //}

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a == y, x > y
TEST_F(IResearchQueryOptimizationTest, test_3) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'C', 'B' ] AND d.values == 'A' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      }
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a == y, x < y
TEST_F(IResearchQueryOptimizationTest, test_4) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ '@', 'A' ] AND d.values != 'D' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("@");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
      }
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a == y, x < y
TEST_F(IResearchQueryOptimizationTest, test_5) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ '@', 'A' ] AND d.values != 'B' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("@");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
      }
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a == y, x > y
TEST_F(IResearchQueryOptimizationTest, test_6) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'C', 'D' ] AND d.values != 'D' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // FIXME
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
      }
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
    }
    //{
    //  irs::Or expected;
    //  auto& root = expected.add<irs::And>();
    //  {
    //    auto& sub = root.add<irs::Or>();
    //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
    //  }
    //  root.add<irs::Not>().filter<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    //}

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  /*
  //FIXME
  // a IN [ x ] && a == y, x == y
TEST_F(IResearchQueryOptimizationTest, test_7) {
    std::string const query =
      "FOR d IN testView SEARCH d.values IN [ 'A', 'A' ] AND d.values != 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(
    vocbase(), query, {
        arangodb::aql::OptimizerRule::handleArangoSearchViewsRule
      }
    ));

  EXPECT_TRUE(findEmptyNodes(vocbase(), query));

    std::vector<arangodb::velocypack::Slice> expectedDocs {
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (;resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }
  */

  // a IN [ x ] && a != y, x > y
TEST_F(IResearchQueryOptimizationTest, test_8) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'C', 'B' ] AND d.values != 'A' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      }
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a != y, x > y
TEST_F(IResearchQueryOptimizationTest, test_9) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'C', 'B' ] AND d.values != '@' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      }
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("@");
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a < y, x < y
TEST_F(IResearchQueryOptimizationTest, test_10) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'A', 'B' ] AND d.values < 'C' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // FIXME
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      }
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("C");
    }
    //{
    //  irs::Or expected;
    //  {
    //    auto& sub = root.add<irs::Or>();
    //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    //  }
    //}

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a < y, x == y
TEST_F(IResearchQueryOptimizationTest, test_11) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'A', 'C' ] AND d.values < 'C' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      }
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("C");
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a < y, x > y
TEST_F(IResearchQueryOptimizationTest, test_12) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'D', 'C' ] AND d.values < 'B' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("D");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      }
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a <= y, x < y
TEST_F(IResearchQueryOptimizationTest, test_13) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'B', 'C' ] AND d.values <= 'D' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // FIXME
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      }
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("D");
    }
    //{
    //  irs::Or expected;
    //  auto& root = expected.add<irs::And>();
    //  {
    //    auto& sub = root.add<irs::Or>();
    //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("D");
    //  }
    //}

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a <= y, x == y
TEST_F(IResearchQueryOptimizationTest, test_14) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'B', 'C' ] AND d.values <= 'C' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // FIXME
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      }
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("C");
    }
    //{
    //  irs::Or expected;
    //  auto& root = expected.add<irs::And>();
    //  {
    //    auto& sub = root.add<irs::Or>();
    //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
    //  }
    //}

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a <= y, x > y
TEST_F(IResearchQueryOptimizationTest, test_15) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'B', 'C' ] AND d.values <= 'A' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      }
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("A");
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a >= y, x < y
TEST_F(IResearchQueryOptimizationTest, test_16) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ '@', 'A' ] AND d.values >= 'B' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("@");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
      }
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a >= y, x == y
TEST_F(IResearchQueryOptimizationTest, test_17) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ '@', 'A' ] AND d.values >= 'A' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // FIXME
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("@");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
      }
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("A");
    }
    //{
    //  irs::Or expected;
    //  auto& root = expected.add<irs::And>();
    //  root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    //}

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a >= y, x > y
TEST_F(IResearchQueryOptimizationTest, test_18) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'C', 'D' ] AND d.values >= 'B' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // FIXME
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("D");
      }
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
    }
    //{
    //  irs::Or expected;
    //  auto& root = expected.add<irs::And>();
    //  {
    //    auto& sub = root.add<irs::Or>();
    //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
    //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("D");
    //  }
    //}

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a > y, x < y
TEST_F(IResearchQueryOptimizationTest, test_19) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ '@', 'A' ] AND d.values > 'B' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("@");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
      }
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a > y, x == y
TEST_F(IResearchQueryOptimizationTest, test_20) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'C', 'B' ] AND d.values > 'B' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // FIXME
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      }
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
    }
    //{
    //  irs::Or expected;
    //  auto& root = expected.add<irs::And>();
    //  root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
    //}

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a > y, x > y
TEST_F(IResearchQueryOptimizationTest, test_21) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'C', 'D' ] AND d.values > 'B' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // FIXME
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("D");
      }
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
    }
    //{
    //  irs::Or expected;
    //  auto& root = expected.add<irs::And>();
    //  {
    //    auto& sub = root.add<irs::Or>();
    //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
    //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("D");
    //  }
    //}

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a IN [ y ]
TEST_F(IResearchQueryOptimizationTest, test_22) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'A', 'B' ] AND d.values IN [ "
        "'A', 'B', 'C' ] RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // FIXME optimize
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      }
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      }
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a == y, x < y
TEST_F(IResearchQueryOptimizationTest, test_23) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'B' ] AND d.values == 'C' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a == y, x == y
TEST_F(IResearchQueryOptimizationTest, test_24) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'C' ] AND d.values == 'C' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a == y, x > y
TEST_F(IResearchQueryOptimizationTest, test_25) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'C' ] AND d.values == 'B' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a != y, x < y
TEST_F(IResearchQueryOptimizationTest, test_26) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'A' ] AND d.values != 'B' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a != y, x == y
TEST_F(IResearchQueryOptimizationTest, test_27) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'C' ] AND d.values != 'C' "
        "RETURN d";

    // FIXME
    // EXPECT_TRUE(arangodb::tests::assertRules(
  //  vocbase(), query, {
    //    arangodb::aql::OptimizerRule::handleArangoSearchViewsRule
    //  }
    //));

  EXPECT_TRUE(findEmptyNodes(vocbase(), query));

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a != y, x > y
TEST_F(IResearchQueryOptimizationTest, test_28) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN ['B'] AND d.values != 'C' RETURN "
        "d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("C");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a < y, x < y
TEST_F(IResearchQueryOptimizationTest, test_29) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'B' ] AND d.values < 'C' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a < y, x == y
TEST_F(IResearchQueryOptimizationTest, test_30) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'C' ] AND d.values < 'C' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("C");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a < y, x > y
TEST_F(IResearchQueryOptimizationTest, test_31) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'C' ] AND d.values < 'B' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a <= y, x < y
TEST_F(IResearchQueryOptimizationTest, test_32) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'B' ] AND d.values <= 'C' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [x] && a <= y, x == y
TEST_F(IResearchQueryOptimizationTest, test_33) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'B' ] AND d.values <= 'B' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a <= y, x > y
TEST_F(IResearchQueryOptimizationTest, test_34) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'C' ] AND d.values <= 'B' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a >= y, x < y
TEST_F(IResearchQueryOptimizationTest, test_35) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'A' ] AND d.values >= 'B' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a >= y, x == y
TEST_F(IResearchQueryOptimizationTest, test_36) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'B' ] AND d.values >= 'B' "
        "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [x] && a >= y, x > y
TEST_F(IResearchQueryOptimizationTest, test_37) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN ['C'] AND d.values >= 'B' RETURN "
        "d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [x] && a > y, x < y
TEST_F(IResearchQueryOptimizationTest, test_38) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN ['A'] AND d.values > 'B' RETURN "
        "d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [x] && a > y, x == y
TEST_F(IResearchQueryOptimizationTest, test_39) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN ['B'] AND d.values > 'B' RETURN "
        "d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [x] && a > y, x > y
TEST_F(IResearchQueryOptimizationTest, test_40) {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN ['C'] AND d.values > 'B' RETURN "
        "d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a == y, x < y
TEST_F(IResearchQueryOptimizationTest, test_41) {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'B' AND d.values == 'C' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a == y, x == y
TEST_F(IResearchQueryOptimizationTest, test_42) {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'C' AND d.values == 'C' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a == y, x > y
TEST_F(IResearchQueryOptimizationTest, test_43) {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'C' AND d.values == 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a != y, x < y
TEST_F(IResearchQueryOptimizationTest, test_44) {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'A' AND d.values != 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a != y, x == y
TEST_F(IResearchQueryOptimizationTest, test_45) {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'C' AND d.values != 'C' RETURN d";

    // FIXME
    // EXPECT_TRUE(arangodb::tests::assertRules(
  //  vocbase(), query, {
    //    arangodb::aql::OptimizerRule::handleArangoSearchViewsRule
    //  }
    //));

  EXPECT_TRUE(findEmptyNodes(vocbase(), query));

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a != y, x > y
TEST_F(IResearchQueryOptimizationTest, test_46) {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'B' AND d.values != 'C' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("C");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a < y, x < y
TEST_F(IResearchQueryOptimizationTest, test_47) {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'B' AND d.values < 'C' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a < y, x == y
TEST_F(IResearchQueryOptimizationTest, test_48) {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'C' AND d.values < 'C' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("C");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a < y, x > y
TEST_F(IResearchQueryOptimizationTest, test_49) {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'C' AND d.values < 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a <= y, x < y
TEST_F(IResearchQueryOptimizationTest, test_50) {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'B' AND d.values <= 'C' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a <= y, x == y
TEST_F(IResearchQueryOptimizationTest, test_51) {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'B' AND d.values <= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a <= y, x > y
TEST_F(IResearchQueryOptimizationTest, test_52) {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'C' AND d.values <= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a >= y, x < y
TEST_F(IResearchQueryOptimizationTest, test_53) {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'A' AND d.values >= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a >= y, x == y
TEST_F(IResearchQueryOptimizationTest, test_54) {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'B' AND d.values >= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a >= y, x > y
TEST_F(IResearchQueryOptimizationTest, test_55) {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'C' AND d.values >= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a > y, x < y
TEST_F(IResearchQueryOptimizationTest, test_56) {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'A' AND d.values > 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a > y, x == y
TEST_F(IResearchQueryOptimizationTest, test_57) {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'B' AND d.values > 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a > y, x > y
TEST_F(IResearchQueryOptimizationTest, test_58) {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'C' AND d.values > 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a == y, x < y
TEST_F(IResearchQueryOptimizationTest, test_59) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != '@' AND d.values == 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("@");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a == y, x < y
TEST_F(IResearchQueryOptimizationTest, test_60) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'A' AND d.values == 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a == y, x == y
TEST_F(IResearchQueryOptimizationTest, test_61) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'A' AND d.values == 'A' RETURN d";

    // FIXME
    // EXPECT_TRUE(arangodb::tests::assertRules(
  //  vocbase(), query, {
    //    arangodb::aql::OptimizerRule::handleArangoSearchViewsRule
    //  }
    //));

  EXPECT_TRUE(findEmptyNodes(vocbase(), query));

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a == y, x > y
TEST_F(IResearchQueryOptimizationTest, test_62) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'D' AND d.values == 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a == y, x > y
TEST_F(IResearchQueryOptimizationTest, test_63) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'B' AND d.values == 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a != y, x < y
TEST_F(IResearchQueryOptimizationTest, test_64) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != '@' AND d.values != 'D' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("@");
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a != y, x < y
TEST_F(IResearchQueryOptimizationTest, test_65) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'A' AND d.values != 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a != y, x == y
TEST_F(IResearchQueryOptimizationTest, test_66) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'D' AND d.values != 'D' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a != y, x == y
TEST_F(IResearchQueryOptimizationTest, test_67) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'A' AND d.values != 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a != y, x > y
TEST_F(IResearchQueryOptimizationTest, test_68) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'B' AND d.values != 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a < y, x < y
TEST_F(IResearchQueryOptimizationTest, test_69) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != '0' AND d.values < 'C' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("0");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("C");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a < y, x == y
TEST_F(IResearchQueryOptimizationTest, test_70) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'A' AND d.values < 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a < y, x == y
TEST_F(IResearchQueryOptimizationTest, test_71) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != '@' AND d.values < 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("@");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a < y, x == y
TEST_F(IResearchQueryOptimizationTest, test_72) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'D' AND d.values < 'D' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("D");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a < y, x > y
TEST_F(IResearchQueryOptimizationTest, test_73) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'D' AND d.values < 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a < y, x > y
TEST_F(IResearchQueryOptimizationTest, test_74) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'C' AND d.values < 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a <= y, x < y
TEST_F(IResearchQueryOptimizationTest, test_75) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != '0' AND d.values <= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("0");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a <= y, x < y
TEST_F(IResearchQueryOptimizationTest, test_76) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'A' AND d.values <= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a <= y, x == y
TEST_F(IResearchQueryOptimizationTest, test_77) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'D' AND d.values <= 'D' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("D");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a <= y, x == y
TEST_F(IResearchQueryOptimizationTest, test_78) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'B' AND d.values <= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a <= y, x > y
TEST_F(IResearchQueryOptimizationTest, test_79) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'D' AND d.values <= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a <= y, x > y
TEST_F(IResearchQueryOptimizationTest, test_80) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'C' AND d.values <= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a >= y, x < y
TEST_F(IResearchQueryOptimizationTest, test_81) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != '0' AND d.values >= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("0");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a >= y, x < y
TEST_F(IResearchQueryOptimizationTest, test_82) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'A' AND d.values >= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a >= y, x == y
TEST_F(IResearchQueryOptimizationTest, test_83) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != '0' AND d.values >= '0' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("0");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("0");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a >= y, x == y
TEST_F(IResearchQueryOptimizationTest, test_84) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'A' AND d.values >= 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a >= y, x > y
TEST_F(IResearchQueryOptimizationTest, test_85) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'D' AND d.values >= 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a >= y, x > y
TEST_F(IResearchQueryOptimizationTest, test_86) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'C' AND d.values >= 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a > y, x < y
TEST_F(IResearchQueryOptimizationTest, test_87) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != '0' AND d.values > 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("0");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a > y, x < y
TEST_F(IResearchQueryOptimizationTest, test_88) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'A' AND d.values > 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a > y, x == y
TEST_F(IResearchQueryOptimizationTest, test_89) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != '0' AND d.values > '0' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("0");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("0");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a > y, x == y
TEST_F(IResearchQueryOptimizationTest, test_90) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'A' AND d.values > 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a > y, x > y
TEST_F(IResearchQueryOptimizationTest, test_91) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'D' AND d.values > 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a > y, x > y
TEST_F(IResearchQueryOptimizationTest, test_92) {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'C' AND d.values > 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a == y, x < y
TEST_F(IResearchQueryOptimizationTest, test_93) {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'B' AND d.values == 'C' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a == y, x == y
TEST_F(IResearchQueryOptimizationTest, test_94) {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'B' AND d.values == 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a == y, x > y
TEST_F(IResearchQueryOptimizationTest, test_95) {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'C' AND d.values == 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a != y, x < y
TEST_F(IResearchQueryOptimizationTest, test_96) {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'B' AND d.values != 'D' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a != y, x < y
TEST_F(IResearchQueryOptimizationTest, test_97) {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'B' AND d.values != 'C' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a != y, x == y
TEST_F(IResearchQueryOptimizationTest, test_98) {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'D' AND d.values != 'D' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("D");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a != y, x == y
TEST_F(IResearchQueryOptimizationTest, test_99) {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'B' AND d.values != 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a != y, x > y
TEST_F(IResearchQueryOptimizationTest, test_100) {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'C' AND d.values != '0' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("0");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("C");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a != y, x > y
TEST_F(IResearchQueryOptimizationTest, test_101) {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'C' AND d.values != 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("C");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a < y, x < y
TEST_F(IResearchQueryOptimizationTest, test_102) {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'B' AND d.values < 'C' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a < y, x == y
TEST_F(IResearchQueryOptimizationTest, test_103) {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'B' AND d.values < 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a < y, x > y
TEST_F(IResearchQueryOptimizationTest, test_104) {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'C' AND d.values < 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a <= y, x < y
TEST_F(IResearchQueryOptimizationTest, test_105) {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'B' AND d.values <= 'C' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a <= y, x == y
TEST_F(IResearchQueryOptimizationTest, test_106) {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'C' AND d.values <= 'C' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("C");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a <= y, x > y
TEST_F(IResearchQueryOptimizationTest, test_107) {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'C' AND d.values <= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a >= y, x < y
TEST_F(IResearchQueryOptimizationTest, test_108) {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'B' AND d.values >= 'C' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a >= y, x == y
TEST_F(IResearchQueryOptimizationTest, test_109) {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'B' AND d.values >= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a >= y, x > y
TEST_F(IResearchQueryOptimizationTest, test_110) {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'C' AND d.values >= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("C");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a > y, x < y
TEST_F(IResearchQueryOptimizationTest, test_111) {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'B' AND d.values > 'C' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a > y, x == y
TEST_F(IResearchQueryOptimizationTest, test_112) {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'B' AND d.values > 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a > y, x > y
TEST_F(IResearchQueryOptimizationTest, test_113) {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'C' AND d.values > 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("C");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a == y, x < y
TEST_F(IResearchQueryOptimizationTest, test_114) {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'A' AND d.values == 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a == y, x == y
TEST_F(IResearchQueryOptimizationTest, test_115) {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'A' AND d.values == 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a == y, x > y
TEST_F(IResearchQueryOptimizationTest, test_116) {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'B' AND d.values == 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a != y, x < y
TEST_F(IResearchQueryOptimizationTest, test_117) {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'A' AND d.values != 'D' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a != y, x < y
TEST_F(IResearchQueryOptimizationTest, test_118) {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'A' AND d.values != 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a != y, x == y
TEST_F(IResearchQueryOptimizationTest, test_119) {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'B' AND d.values != 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a != y, x == y
TEST_F(IResearchQueryOptimizationTest, test_120) {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'D' AND d.values != 'D' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("D");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a != y, x > y
TEST_F(IResearchQueryOptimizationTest, test_121) {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'C' AND d.values != '@' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("@");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("C");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a != y, x > y
TEST_F(IResearchQueryOptimizationTest, test_122) {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'C' AND d.values != 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("C");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a < y, x < y
TEST_F(IResearchQueryOptimizationTest, test_123) {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'A' AND d.values < 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a < y, x == y
TEST_F(IResearchQueryOptimizationTest, test_124) {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'B' AND d.values < 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a < y, x > y
TEST_F(IResearchQueryOptimizationTest, test_125) {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'C' AND d.values < 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a <= y, x < y
TEST_F(IResearchQueryOptimizationTest, test_126) {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'A' AND d.values <= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a <= y, x == y
TEST_F(IResearchQueryOptimizationTest, test_127) {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'B' AND d.values <= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a <= y, x > y
TEST_F(IResearchQueryOptimizationTest, test_128) {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'C' AND d.values <= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a >= y, x < y
TEST_F(IResearchQueryOptimizationTest, test_129) {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'A' AND d.values >= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a >= y, x == y
TEST_F(IResearchQueryOptimizationTest, test_130) {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'A' AND d.values >= 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a >= y, x > y
TEST_F(IResearchQueryOptimizationTest, test_131) {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'C' AND d.values >= 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("C");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a > y, x < y
TEST_F(IResearchQueryOptimizationTest, test_132) {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'A' AND d.values > 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a > y, x == y
TEST_F(IResearchQueryOptimizationTest, test_133) {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'A' AND d.values > 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a > y, x > y
TEST_F(IResearchQueryOptimizationTest, test_134) {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'C' AND d.values > 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("C");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a == y, x < y
TEST_F(IResearchQueryOptimizationTest, test_135) {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'A' AND d.values == 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a == y, x == y
TEST_F(IResearchQueryOptimizationTest, test_136) {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'A' AND d.values == 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a == y, x > y
TEST_F(IResearchQueryOptimizationTest, test_137) {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'B' AND d.values == 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a != y, x < y
TEST_F(IResearchQueryOptimizationTest, test_138) {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'A' AND d.values != 'D' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a != y, x < y
TEST_F(IResearchQueryOptimizationTest, test_139) {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'A' AND d.values != 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a != y, x == y
TEST_F(IResearchQueryOptimizationTest, test_140) {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= '@' AND d.values != '@' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("@");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("@");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a != y, x == y
TEST_F(IResearchQueryOptimizationTest, test_141) {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'A' AND d.values != 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a != y, x > y
TEST_F(IResearchQueryOptimizationTest, test_142) {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'B' AND d.values != 'D' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a != y, x > y
TEST_F(IResearchQueryOptimizationTest, test_143) {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'B' AND d.values != 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a < y, x < y
TEST_F(IResearchQueryOptimizationTest, test_144) {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'A' AND d.values < 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a < y, x == y
TEST_F(IResearchQueryOptimizationTest, test_145) {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'B' AND d.values < 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a < y, x > y
TEST_F(IResearchQueryOptimizationTest, test_146) {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'C' AND d.values < 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a <= y, x < y
TEST_F(IResearchQueryOptimizationTest, test_147) {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'A' AND d.values <= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a <= y, x == y
TEST_F(IResearchQueryOptimizationTest, test_148) {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'B' AND d.values <= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a <= y, x > y
TEST_F(IResearchQueryOptimizationTest, test_149) {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'C' AND d.values <= 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a >= y, x < y
TEST_F(IResearchQueryOptimizationTest, test_150) {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'A' AND d.values >= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a >= y, x == y
TEST_F(IResearchQueryOptimizationTest, test_151) {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'B' AND d.values >= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a >= y, x > y
TEST_F(IResearchQueryOptimizationTest, test_152) {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'C' AND d.values >= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("C");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a > y, x < y
TEST_F(IResearchQueryOptimizationTest, test_153) {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'A' AND d.values > 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a > y, x == y
TEST_F(IResearchQueryOptimizationTest, test_154) {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'B' AND d.values > 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a > y, x > y
TEST_F(IResearchQueryOptimizationTest, test_155) {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'B' AND d.values > 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a == y, x < y
TEST_F(IResearchQueryOptimizationTest, test_156) {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'A' AND d.values == 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a == y, x == y
TEST_F(IResearchQueryOptimizationTest, test_157) {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'B' AND d.values == 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a == y, x > y
TEST_F(IResearchQueryOptimizationTest, test_158) {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'B' AND d.values == 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a != y, x < y
TEST_F(IResearchQueryOptimizationTest, test_159) {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'A' AND d.values != 'D' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a != y, x < y
TEST_F(IResearchQueryOptimizationTest, test_160) {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'A' AND d.values != 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a != y, x == y
TEST_F(IResearchQueryOptimizationTest, test_161) {
    std::string const query =
        "FOR d IN testView SEARCH d.values > '@' AND d.values != '@' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("@");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("@");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a != y, x == y
TEST_F(IResearchQueryOptimizationTest, test_162) {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'A' AND d.values != 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a != y, x > y
TEST_F(IResearchQueryOptimizationTest, test_163) {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'B' AND d.values != '@' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("@");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a != y, x > y
TEST_F(IResearchQueryOptimizationTest, test_164) {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'B' AND d.values != 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a < y, x < y
TEST_F(IResearchQueryOptimizationTest, test_165) {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'A' AND d.values < 'C' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("C");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a < y, x == y
TEST_F(IResearchQueryOptimizationTest, test_166) {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'B' AND d.values < 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a < y, x > y
TEST_F(IResearchQueryOptimizationTest, test_167) {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'C' AND d.values < 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a <= y, x < y
TEST_F(IResearchQueryOptimizationTest, test_168) {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'A' AND d.values <= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a <= y, x == y
TEST_F(IResearchQueryOptimizationTest, test_169) {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'B' AND d.values <= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a <= y, x > y
TEST_F(IResearchQueryOptimizationTest, test_170) {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'B' AND d.values <= 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("A");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a >= y, x < y
TEST_F(IResearchQueryOptimizationTest, test_171) {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'A' AND d.values >= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a >= y, x == y
TEST_F(IResearchQueryOptimizationTest, test_172) {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'B' AND d.values >= 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a >= y, x > y
TEST_F(IResearchQueryOptimizationTest, test_173) {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'B' AND d.values >= 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a > y, x < y
TEST_F(IResearchQueryOptimizationTest, test_174) {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'A' AND d.values > 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a > y, x == y
TEST_F(IResearchQueryOptimizationTest, test_175) {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'B' AND d.values > 'B' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a > y, x > y
TEST_F(IResearchQueryOptimizationTest, test_176) {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'B' AND d.values > 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
    assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }
