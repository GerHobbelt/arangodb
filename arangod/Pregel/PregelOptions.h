////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
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
/// @author Julia Volmer
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <string>
#include <variant>
#include "Cluster/ClusterTypes.h"
#include "Inspection/Types.h"
#include "velocypack/Builder.h"

namespace arangodb::pregel {

using VertexCollectionID = CollectionID;
using EdgeCollectionID = CollectionID;
using VertexShardID = ShardID;
using EdgeShardID = ShardID;

struct GraphCollectionNames {
  std::vector<std::string> vertexCollections;
  std::vector<std::string> edgeCollections;
};
template<typename Inspector>
auto inspect(Inspector& f, GraphCollectionNames& x) {
  return f.object(x).fields(f.field("vertexCollections", x.vertexCollections),
                            f.field("edgeCollections", x.edgeCollections));
}

struct GraphName {
  std::string graph;
};
template<typename Inspector>
auto inspect(Inspector& f, GraphName& x) {
  return f.object(x).fields(f.field("graph", x.graph));
}

/**
Maps from vertex collection name to a list of edge collections that this
vertex collection is restricted to.
It is only used for a collection if there is at least one entry for the
collection!
 **/
struct EdgeCollectionRestrictions {
  std::unordered_map<VertexCollectionID, std::vector<EdgeCollectionID>> items;
  auto add(EdgeCollectionRestrictions others) const
      -> EdgeCollectionRestrictions;
};
template<typename Inspector>
auto inspect(Inspector& f, EdgeCollectionRestrictions& x) {
  return f.object(x).fields(f.field("items", x.items));
}

struct GraphOrCollection : std::variant<GraphCollectionNames, GraphName> {};
template<class Inspector>
auto inspect(Inspector& f, GraphOrCollection& x) {
  return f.variant(x).unqualified().alternatives(
      arangodb::inspection::type<GraphCollectionNames>("collectionNames"),
      arangodb::inspection::type<GraphName>("graphName"));
}

struct GraphSource {
  GraphOrCollection graphOrCollections;
  EdgeCollectionRestrictions edgeCollectionRestrictions;
};
template<typename Inspector>
auto inspect(Inspector& f, GraphSource& x) {
  return f.object(x).fields(
      f.field("graphOrCollection", x.graphOrCollections),
      f.field("edgeCollectionRestrictions", x.edgeCollectionRestrictions));
}

struct PregelOptions {
  std::string algorithm;
  VPackBuilder userParameters;
  GraphSource graphSource;
};

}  // namespace arangodb::pregel
