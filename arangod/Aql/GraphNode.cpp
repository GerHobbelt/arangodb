////////////////////////////////////////////////////////////////////////////////
/// @brief Generic Node for Graph operations in AQL
///
/// @file arangod/Aql/GraphNode.cpp
///
/// DISCLAIMER
///
/// Copyright 2010-2014 triagens GmbH, Cologne, Germany
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
/// @author Michael Hackstein
/// @author Copyright 2017, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "GraphNode.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/Ast.h"
#include "Aql/Collection.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionNodeId.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Expression.h"
#include "Aql/RegisterPlan.h"
#include "Aql/SingleRowFetcher.h"
#include "Aql/SortCondition.h"
#include "Aql/TraversalExecutor.h"
#include "Aql/Variable.h"
#include "Basics/tryEmplaceHelper.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterTraverser.h"
#include "Cluster/ServerState.h"
#include "Graph/BaseOptions.h"
#include "Graph/Graph.h"
#include "Graph/SingleServerTraverser.h"
#include "Graph/TraverserOptions.h"
#include "Utils/CollectionNameResolver.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ticks.h"
#ifdef USE_ENTERPRISE
#include "Enterprise/Aql/LocalKShortestPathsNode.h"
#include "Enterprise/Aql/LocalShortestPathNode.h"
#include "Enterprise/Aql/LocalTraversalNode.h"
#endif

#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>
#include <utility>

using namespace arangodb;
using namespace arangodb::aql;
using namespace arangodb::graph;
using namespace arangodb::traverser;

static TRI_edge_direction_e uint64ToDirection(uint64_t dirNum) {
  switch (dirNum) {
    case 0:
      return TRI_EDGE_ANY;
    case 1:
      return TRI_EDGE_IN;
    case 2:
      return TRI_EDGE_OUT;
    default:
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_QUERY_PARSE,
          "direction can only be INBOUND, OUTBOUND or ANY");
  }
}

static TRI_edge_direction_e parseDirection(AstNode const* node) {
  TRI_ASSERT(node->isIntValue());
  auto dirNum = node->getIntValue();

  return uint64ToDirection(dirNum);
}

GraphNode::GraphNode(ExecutionPlan* plan, ExecutionNodeId id, TRI_vocbase_t* vocbase,
                     AstNode const* direction, AstNode const* graph,
                     std::unique_ptr<BaseOptions> options)
    : ExecutionNode(plan, id),
      _vocbase(vocbase),
      _vertexOutVariable(nullptr),
      _edgeOutVariable(nullptr),
      _graphObj(nullptr),
      _tmpObjVariable(_plan->getAst()->variables()->createTemporaryVariable()),
      _tmpObjVarNode(_plan->getAst()->createNodeReference(_tmpObjVariable)),
      _tmpIdNode(_plan->getAst()->createNodeValueString("", 0)),
      _defaultDirection(parseDirection(direction)),
      _options(std::move(options)),
      _optionsBuilt(false),
      _isSmart(false) {
  // Direction is already the correct Integer.
  // Is not inserted by user but by enum.

  TRI_ASSERT(_vocbase != nullptr);
  TRI_ASSERT(_options != nullptr);

  TRI_ASSERT(direction != nullptr);
  TRI_ASSERT(graph != nullptr);

  if (graph->type == NODE_TYPE_COLLECTION_LIST) {
    size_t edgeCollectionCount = graph->numMembers();

    _graphInfo.openArray();
    _edgeColls.reserve(edgeCollectionCount);
    _directions.reserve(edgeCollectionCount);

    // First determine whether all edge collections are smart and sharded
    // like a common collection:
    auto& ci = _vocbase->server().getFeature<ClusterFeature>().clusterInfo();
    if (ServerState::instance()->isRunningInCluster()) {
      _isSmart = true;
      std::string distributeShardsLike;
      for (size_t i = 0; i < edgeCollectionCount; ++i) {
        auto col = graph->getMember(i);
        if (col->type == NODE_TYPE_DIRECTION) {
          col = col->getMember(1);  // The first member always is the collection
        }
        std::string n = col->getString();
        auto c = ci.getCollection(_vocbase->name(), n);
        if (!c->isSmart() || c->distributeShardsLike().empty()) {
          _isSmart = false;
          break;
        }
        if (distributeShardsLike.empty()) {
          distributeShardsLike = c->distributeShardsLike();
        } else if (distributeShardsLike != c->distributeShardsLike()) {
          _isSmart = false;
          break;
        }
      }
    }

    std::unordered_map<std::string, TRI_edge_direction_e> seenCollections;
    CollectionNameResolver const* resolver = plan->getAst()->query()->trx()->resolver();

    // List of edge collection names
    for (size_t i = 0; i < edgeCollectionCount; ++i) {
      auto col = graph->getMember(i);
      TRI_edge_direction_e dir = TRI_EDGE_ANY;

      if (col->type == NODE_TYPE_DIRECTION) {
        // We have a collection with special direction.
        dir = parseDirection(col->getMember(0));
        col = col->getMember(1);
      } else {
        dir = _defaultDirection;
      }

      std::string eColName = col->getString();

      if (_options->shouldExcludeEdgeCollection(eColName)) {
        // excluded edge collection
        continue;
      }

      // now do some uniqueness checks for the specified collections
      auto [it, inserted] = seenCollections.try_emplace(eColName, dir);
      if (!inserted) {
        if ((*it).second != dir) {
          std::string msg("conflicting directions specified for collection '" +
                          std::string(eColName));
          THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_COLLECTION_TYPE_INVALID, msg);
        }
        // do not re-add the same collection!
        continue;
      }

      auto collection = resolver->getCollection(eColName);

      if (!collection || collection->type() != TRI_COL_TYPE_EDGE) {
        std::string msg("collection type invalid for collection '" + std::string(eColName) +
                        ": expecting collection type 'edge'");
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_COLLECTION_TYPE_INVALID, msg);
      }

      auto collections = plan->getAst()->query()->collections();

      _graphInfo.add(VPackValue(eColName));
      if (ServerState::instance()->isRunningInCluster()) {
        auto c = ci.getCollection(_vocbase->name(), eColName);
        if (!c->isSmart()) {
          auto aqlCollection = collections->get(eColName);
          addEdgeCollection(aqlCollection, dir);
        } else {
          std::vector<std::string> names;
          if (_isSmart) {
            names = c->realNames();
          } else {
            names = c->realNamesForRead();
          }
          for (auto const& name : names) {
            auto aqlCollection = collections->get(name);
            addEdgeCollection(aqlCollection, dir);
          }
        }
      } else {
        auto aqlCollection = collections->get(eColName);
        addEdgeCollection(aqlCollection, dir);
      }
    }
    _graphInfo.close();
  } else if (graph->isStringValue()) {
    std::string graphName = graph->getString();
    _graphInfo.add(VPackValue(graphName));
    _graphObj = plan->getAst()->query()->lookupGraphByName(graphName);

    if (_graphObj == nullptr) {
      THROW_ARANGO_EXCEPTION_PARAMS(TRI_ERROR_GRAPH_NOT_FOUND, graphName.c_str());
    }

    auto eColls = _graphObj->edgeCollections();
    size_t length = eColls.size();
    if (length == 0) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_GRAPH_EMPTY);
    }

    // First determine whether all edge collections are smart and sharded
    // like a common collection:
    auto& ci = _vocbase->server().getFeature<ClusterFeature>().clusterInfo();
    if (ServerState::instance()->isRunningInCluster()) {
      _isSmart = true;
      std::string distributeShardsLike;
      for (auto const& n : eColls) {
        auto c = ci.getCollection(_vocbase->name(), n);
        if (!c->isSmart() || c->distributeShardsLike().empty()) {
          _isSmart = false;
          break;
        }
        if (distributeShardsLike.empty()) {
          distributeShardsLike = c->distributeShardsLike();
        } else if (distributeShardsLike != c->distributeShardsLike()) {
          _isSmart = false;
          break;
        }
      }
    }

    for (const auto& n : eColls) {
      if (_options->shouldExcludeEdgeCollection(n)) {
        // excluded edge collection
        continue;
      }

      auto collections = plan->getAst()->query()->collections();
      if (ServerState::instance()->isRunningInCluster()) {
        auto c = ci.getCollection(_vocbase->name(), n);
        if (!c->isSmart()) {
          auto aqlCollection = collections->get(n);
          addEdgeCollection(aqlCollection, _defaultDirection);
        } else {
          std::vector<std::string> names;
          if (_isSmart) {
            names = c->realNames();
          } else {
            names = c->realNamesForRead();
          }
          for (auto const& name : names) {
            auto aqlCollection = collections->get(name);
            addEdgeCollection(aqlCollection, _defaultDirection);
          }
        }
      } else {
        auto aqlCollection = collections->get(n);
        addEdgeCollection(aqlCollection, _defaultDirection);
      }
    }

    auto collections = plan->getAst()->query()->collections();
    auto vColls = _graphObj->vertexCollections();
    length = vColls.size();
    if (length == 0) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_GRAPH_EMPTY);
    }
    _vertexColls.reserve(length);
    for (auto const& v : vColls) {
      auto aqlCollection = collections->get(v);
      addVertexCollection(aqlCollection);
    }
  }
}

GraphNode::GraphNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base)
    : ExecutionNode(plan, base),
      _vocbase(&(plan->getAst()->query()->vocbase())),
      _vertexOutVariable(nullptr),
      _edgeOutVariable(nullptr),
      _graphObj(nullptr),
      _tmpObjVariable(nullptr),
      _tmpObjVarNode(nullptr),
      _tmpIdNode(nullptr),
      _defaultDirection(uint64ToDirection(arangodb::basics::VelocyPackHelper::stringUInt64(
          base.get("defaultDirection")))),
      _optionsBuilt(false),
      _isSmart(false) {
  auto thread_local const isDBServer = ServerState::instance()->isDBServer();

  if (!isDBServer) {
    // Graph Information. Do we need to reload the graph here?
    std::string graphName;
    if (base.hasKey("graph") && (base.get("graph").isString())) {
      graphName = base.get("graph").copyString();
      if (base.hasKey("graphDefinition")) {
        // load graph and store pointer
        _graphObj = plan->getAst()->query()->lookupGraphByName(graphName);

        if (_graphObj == nullptr) {
          THROW_ARANGO_EXCEPTION_PARAMS(TRI_ERROR_GRAPH_NOT_FOUND, graphName.c_str());
        }
      } else {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                       "missing graphDefinition.");
      }
    } else {
      _graphInfo.add(base.get("graph"));
      if (!_graphInfo.slice().isArray()) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                       "graph has to be an array.");
      }
    }
  }

  // Collection information
  VPackSlice edgeCollections = base.get("edgeCollections");
  if (!edgeCollections.isArray()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                   "graph needs an array of edge collections.");
  }
  // Directions
  VPackSlice dirList = base.get("directions");
  if (!dirList.isArray()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
        "graph needs an array of directions.");
  }

  if (edgeCollections.length() != dirList.length()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
        "graph needs the same number of edge collections and directions.");
  }

  auto query = plan->getAst()->query();
  // MSVC did throw an internal compiler error with auto instead of std::string
  // here at the time of this writing (some MSVC 2019 14.25.28610). Could
  // reproduce it only in our CI, my local MSVC (same version) ran fine...
  auto getAqlCollectionFromName = [&](std::string const& name) -> aql::Collection* {
    // if the collection was already existent in the query, addCollection will
    // just return it.
    if (isDBServer) {
      auto shard = collectionToShardName(name);
      return query->addCollection(name, AccessMode::Type::READ);
    } else {
      return query->addCollection(name, AccessMode::Type::READ);
    }
  };

  auto vPackDirListIter = VPackArrayIterator(dirList);
  auto vPackEdgeColIter = VPackArrayIterator(edgeCollections);
  for (auto dirIt = vPackDirListIter.begin(), edgeIt = vPackEdgeColIter.begin();
       dirIt != vPackDirListIter.end() && edgeIt != vPackEdgeColIter.end();
       ++dirIt, ++edgeIt) {
    uint64_t dir = arangodb::basics::VelocyPackHelper::stringUInt64(*dirIt);
    TRI_edge_direction_e d = uint64ToDirection(dir);
    // Only TRI_EDGE_IN and TRI_EDGE_OUT allowed here
    TRI_ASSERT(d == TRI_EDGE_IN || d == TRI_EDGE_OUT);
    std::string e = arangodb::basics::VelocyPackHelper::getStringValue(*edgeIt, "");
    auto* aqlCollection = getAqlCollectionFromName(e);
    addEdgeCollection(aqlCollection, d);
  }

  VPackSlice vertexCollections = base.get("vertexCollections");

  if (!vertexCollections.isArray()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_QUERY_BAD_JSON_PLAN,
        "graph needs an array of vertex collections.");
  }

  for (VPackSlice it : VPackArrayIterator(vertexCollections)) {
    std::string v = arangodb::basics::VelocyPackHelper::getStringValue(it, "");
    auto aqlCollection = getAqlCollectionFromName(v);
    addVertexCollection(aqlCollection);
  }

  // translations for one-shard-databases
  VPackSlice collectionToShard = base.get("collectionToShard");
  if (!collectionToShard.isObject()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_QUERY_BAD_JSON_PLAN,
        "graph needs a translation from collection to shard names");
  }
  for (auto const& item : VPackObjectIterator(collectionToShard)) {
    _collectionToShard.insert({item.key.copyString(), item.value.copyString()});
  }

  // Out variables
  if (base.hasKey("vertexOutVariable")) {
    _vertexOutVariable =
        Variable::varFromVPack(plan->getAst(), base, "vertexOutVariable");
  }
  if (base.hasKey("edgeOutVariable")) {
    _edgeOutVariable =
        Variable::varFromVPack(plan->getAst(), base, "edgeOutVariable");
  }

  // Temporary Filter Objects
  TRI_ASSERT(base.hasKey("tmpObjVariable"));
  _tmpObjVariable =
      Variable::varFromVPack(plan->getAst(), base, "tmpObjVariable");

  TRI_ASSERT(base.hasKey("tmpObjVarNode"));
  // the plan's AST takes ownership of the newly created AstNode, so this is
  // safe cppcheck-suppress *
  _tmpObjVarNode = new AstNode(plan->getAst(), base.get("tmpObjVarNode"));

  TRI_ASSERT(base.hasKey("tmpIdNode"));
  // the plan's AST takes ownership of the newly created AstNode, so this is
  // safe cppcheck-suppress *
  _tmpIdNode = new AstNode(plan->getAst(), base.get("tmpIdNode"));

  VPackSlice opts = base.get("options");
  if (!opts.isObject()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                   "graph options have to be a json-object.");
  }

  _options = BaseOptions::createOptionsFromSlice(_plan->getAst()->query(), opts);
  // set traversal-translations
  _options->setCollectionToShard(_collectionToShard);  // could be moved as it will only be used here
}

/// @brief Internal constructor to clone the node.
GraphNode::GraphNode(ExecutionPlan* plan, ExecutionNodeId id, TRI_vocbase_t* vocbase,
                     std::vector<Collection*> const& edgeColls,
                     std::vector<Collection*> const& vertexColls,
                     TRI_edge_direction_e defaultDirection,
                     std::vector<TRI_edge_direction_e> directions,
                     std::unique_ptr<graph::BaseOptions> options, Graph const* graph)
    : ExecutionNode(plan, id),
      _vocbase(vocbase),
      _vertexOutVariable(nullptr),
      _edgeOutVariable(nullptr),
      _graphObj(graph),
      _tmpObjVariable(_plan->getAst()->variables()->createTemporaryVariable()),
      _tmpObjVarNode(_plan->getAst()->createNodeReference(_tmpObjVariable)),
      _tmpIdNode(_plan->getAst()->createNodeValueString("", 0)),
      _defaultDirection(defaultDirection),
      _directions(std::move(directions)),
      _options(std::move(options)),
      _optionsBuilt(false),
      _isSmart(false) {
  setGraphInfoAndCopyColls(edgeColls, vertexColls);
}

void GraphNode::setGraphInfoAndCopyColls(std::vector<Collection*> const& edgeColls,
                                         std::vector<Collection*> const& vertexColls) {
  _graphInfo.openArray();
  for (auto& it : edgeColls) {
    _edgeColls.emplace_back(it);
    _graphInfo.add(VPackValue(it->name()));
  }
  _graphInfo.close();

  for (auto& it : vertexColls) {
    addVertexCollection(it);
  }
}

GraphNode::GraphNode(ExecutionPlan& plan, GraphNode const& other,
                     std::unique_ptr<graph::BaseOptions> options)
    : ExecutionNode(plan, other),
      _vocbase(other._vocbase),
      _vertexOutVariable(nullptr),
      _edgeOutVariable(nullptr),
      _graphObj(other.graph()),
      _tmpObjVariable(_plan->getAst()->variables()->createTemporaryVariable()),
      _tmpObjVarNode(_plan->getAst()->createNodeReference(_tmpObjVariable)),
      _tmpIdNode(_plan->getAst()->createNodeValueString("", 0)),
      _defaultDirection(other._defaultDirection),
      _directions(other._directions),
      _options(std::move(options)),
      _optionsBuilt(false),
      _isSmart(other.isSmart()),
      _collectionToShard(other._collectionToShard) {
  setGraphInfoAndCopyColls(other.edgeColls(), other.vertexColls());
}

GraphNode::GraphNode(THIS_THROWS_WHEN_CALLED)
    : ExecutionNode(nullptr, ExecutionNodeId{0}), _defaultDirection() {
  TRI_ASSERT(false);
  THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL);
}

std::string const& GraphNode::collectionToShardName(std::string const& collName) const {
  if (_collectionToShard.empty()) {
    return collName;
  }

  auto found = _collectionToShard.find(collName);
  TRI_ASSERT(found != _collectionToShard.cend());
  return found->second;
}

void GraphNode::toVelocyPackHelper(VPackBuilder& nodes, unsigned flags,
                                   std::unordered_set<ExecutionNode const*>& seen) const {
  // call base class method
  ExecutionNode::toVelocyPackHelperGeneric(nodes, flags, seen);

  // Vocbase
  nodes.add("database", VPackValue(_vocbase->name()));

  // TODO We need Both?!
  // Graph definition
  nodes.add("graph", _graphInfo.slice());

  // Graph Definition
  if (_graphObj != nullptr) {
    nodes.add(VPackValue("graphDefinition"));
    _graphObj->toVelocyPack(nodes);
  }

  // Default Direction
  nodes.add("defaultDirection", VPackValue(_defaultDirection));

  // Directions
  nodes.add(VPackValue("directions"));
  {
    VPackArrayBuilder guard(&nodes);
    for (auto const& d : _directions) {
      nodes.add(VPackValue(d));
    }
  }

  // Collections
  nodes.add(VPackValue("edgeCollections"));
  {
    VPackArrayBuilder guard(&nodes);
    for (auto const& e : _edgeColls) {
      nodes.add(VPackValue(collectionToShardName(e->name())));
    }
  }

  nodes.add(VPackValue("vertexCollections"));
  {
    VPackArrayBuilder guard(&nodes);
    for (auto const& v : _vertexColls) {
      nodes.add(VPackValue(collectionToShardName(v->name())));
    }
  }

  // translations for one-shard-databases
  nodes.add(VPackValue("collectionToShard"));
  {
    VPackObjectBuilder guard(&nodes);
    for (auto const& item : _collectionToShard) {
      nodes.add(item.first, VPackValue(item.second));
    }
  }

  // Out variables
  if (usesVertexOutVariable()) {
    nodes.add(VPackValue("vertexOutVariable"));
    vertexOutVariable()->toVelocyPack(nodes);
  }
  if (usesEdgeOutVariable()) {
    nodes.add(VPackValue("edgeOutVariable"));
    edgeOutVariable()->toVelocyPack(nodes);
  }

  // Temporary AST Nodes for conditions
  TRI_ASSERT(_tmpObjVariable != nullptr);
  nodes.add(VPackValue("tmpObjVariable"));
  _tmpObjVariable->toVelocyPack(nodes);

  TRI_ASSERT(_tmpObjVarNode != nullptr);
  nodes.add(VPackValue("tmpObjVarNode"));
  _tmpObjVarNode->toVelocyPack(nodes, flags != 0);

  TRI_ASSERT(_tmpIdNode != nullptr);
  nodes.add(VPackValue("tmpIdNode"));
  _tmpIdNode->toVelocyPack(nodes, flags != 0);

  nodes.add(VPackValue("options"));
  _options->toVelocyPack(nodes);

  nodes.add(VPackValue("indexes"));
  _options->toVelocyPackIndexes(nodes);
}

CostEstimate GraphNode::estimateCost() const {
  CostEstimate estimate = _dependencies.at(0)->getCost();
  size_t incoming = estimate.estimatedNrItems;
  estimate.estimatedCost += incoming * _options->estimateCost(estimate.estimatedNrItems);
  return estimate;
}

void GraphNode::addEngine(TraverserEngineID const& engine, ServerID const& server) {
  TRI_ASSERT(arangodb::ServerState::instance()->isCoordinator());
  _engines.try_emplace(server, engine);
}

/// @brief Returns a reference to the engines. (CLUSTER ONLY)
std::unordered_map<ServerID, traverser::TraverserEngineID> const* GraphNode::engines() const {
  TRI_ASSERT(arangodb::ServerState::instance()->isCoordinator());
  return &_engines;
}

BaseOptions* GraphNode::options() const { return _options.get(); }

AstNode* GraphNode::getTemporaryRefNode() const { return _tmpObjVarNode; }

Variable const* GraphNode::getTemporaryVariable() const {
  return _tmpObjVariable;
}

void GraphNode::getConditionVariables(std::vector<Variable const*>& res) const {
  // No variables used, nothing todo
}

Collection const* GraphNode::collection() const {
  TRI_ASSERT(ServerState::instance()->isCoordinator());
  TRI_ASSERT(!_edgeColls.empty());
  TRI_ASSERT(_edgeColls.front() != nullptr);
  return _edgeColls.front();
}

void GraphNode::injectVertexCollection(aql::Collection* other) {
  TRI_ASSERT(ServerState::instance()->isCoordinator());

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  // This is a workaround to inject all unknown aql collections into
  // this node, that should be list of unique values!
  for (auto const& v : _vertexColls) {
    TRI_ASSERT(v->name() != other->name());
  }
#endif
  addVertexCollection(other);
}

#ifndef USE_ENTERPRISE
void GraphNode::enhanceEngineInfo(VPackBuilder& builder) const {
  if (_graphObj != nullptr) {
    _graphObj->enhanceEngineInfo(builder);
  } else {
    // TODO enhance the Info based on EdgeCollections.
  }
}
#endif

void GraphNode::addEdgeCollection(aql::Collection* collection, TRI_edge_direction_e dir) {
  TRI_ASSERT(collection != nullptr);
  auto const& n = collection->name();
  if (_isSmart) {
    if (n.compare(0, 6, "_from_") == 0) {
      if (dir != TRI_EDGE_IN) {
        _directions.emplace_back(TRI_EDGE_OUT);
        _edgeColls.emplace_back(collection);
      }
      return;
    } else if (n.compare(0, 4, "_to_") == 0) {
      if (dir != TRI_EDGE_OUT) {
        _directions.emplace_back(TRI_EDGE_IN);
        _edgeColls.emplace_back(collection);
      }
      return;
    }
  }

  if (dir == TRI_EDGE_ANY) {
    _directions.emplace_back(TRI_EDGE_OUT);
    _edgeColls.emplace_back(collection);

    _directions.emplace_back(TRI_EDGE_IN);
    _edgeColls.emplace_back(collection);
  } else {
    _directions.emplace_back(dir);
    _edgeColls.emplace_back(collection);
  }
}

void GraphNode::addVertexCollection(aql::Collection* collection) {
  TRI_ASSERT(collection != nullptr);
  _vertexColls.emplace_back(collection);
}

std::vector<aql::Collection const*> GraphNode::collections() const {
  std::vector<aql::Collection const*> rv;
  rv.reserve(_edgeColls.size() + _vertexColls.size());

  for (auto const& collPointer : _edgeColls) {
    rv.push_back(collPointer);
  }
  for (auto const& collPointer : _vertexColls) {
    rv.push_back(collPointer);
  }
  return rv;
}

bool GraphNode::isSmart() const { return _isSmart; }

TRI_vocbase_t* GraphNode::vocbase() const { return _vocbase; }

Variable const* GraphNode::vertexOutVariable() const {
  return _vertexOutVariable;
}

bool GraphNode::usesVertexOutVariable() const {
  return _vertexOutVariable != nullptr && _options->produceVertices();
}

void GraphNode::setVertexOutput(Variable const* outVar) {
  _vertexOutVariable = outVar;
}

Variable const* GraphNode::edgeOutVariable() const { return _edgeOutVariable; }

bool GraphNode::usesEdgeOutVariable() const {
  return _edgeOutVariable != nullptr;
}

void GraphNode::setEdgeOutput(Variable const* outVar) {
  _edgeOutVariable = outVar;
}

std::vector<aql::Collection*> const& GraphNode::edgeColls() const {
  return _edgeColls;
}

std::vector<aql::Collection*> const& GraphNode::vertexColls() const {
  return _vertexColls;
}

graph::Graph const* GraphNode::graph() const noexcept { return _graphObj; }

bool GraphNode::isUsedAsSatellite() const {
#ifndef USE_ENTERPRISE
  return false;
#else
  auto const* collectionAccessingNode =
      dynamic_cast<CollectionAccessingNode const*>(this);
  TRI_ASSERT((collectionAccessingNode != nullptr) ==
             (nullptr != dynamic_cast<LocalTraversalNode const*>(this) ||
              nullptr != dynamic_cast<LocalShortestPathNode const*>(this) ||
              nullptr != dynamic_cast<LocalKShortestPathsNode const*>(this)));
  return collectionAccessingNode != nullptr &&
         collectionAccessingNode->isUsedAsSatellite();
#endif
}

bool GraphNode::isEligibleAsSatelliteTraversal() const {
  return graph() != nullptr && graph()->isSatellite();
}

std::unordered_set<VariableId> GraphNode::getOutputVariables() const {
    std::unordered_set<VariableId> vars;
    for (auto const& it : getVariablesSetHere()) {
        vars.insert(it->id);
    }
    return vars;
}
