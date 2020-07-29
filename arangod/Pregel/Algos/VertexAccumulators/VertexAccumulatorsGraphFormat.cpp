////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2020 ArangoDB GmbH, Cologne, Germany
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
/// @author Heiko Kernbach
/// @author Lars Maier
/// @author Markus Pfeiffer
////////////////////////////////////////////////////////////////////////////////

#include "VertexAccumulators.h"

using namespace arangodb::pregel::algos;

// Graph Format
VertexAccumulators::GraphFormat::GraphFormat(
    application_features::ApplicationServer& server, std::string const& resultField,
    std::unordered_map<std::string, AccumulatorOptions> const& accumulatorDeclarations)
    : graph_format(server),
      _resultField(resultField),
      _accumulatorDeclarations(accumulatorDeclarations){};

size_t VertexAccumulators::GraphFormat::estimatedVertexSize() const {
  return sizeof(vertex_type);
}
size_t VertexAccumulators::GraphFormat::estimatedEdgeSize() const {
  return sizeof(edge_type);
}

// Extract vertex data from vertex document into target
void VertexAccumulators::GraphFormat::copyVertexData(std::string const& documentId,
                                                     arangodb::velocypack::Slice vertexDocument,
                                                     vertex_type& targetPtr) {
  targetPtr.reset(_accumulatorDeclarations, documentId, vertexDocument, _vertexIdRange++);
}

void VertexAccumulators::GraphFormat::copyEdgeData(arangodb::velocypack::Slice edgeDocument,
                                                   edge_type& targetPtr) {
  targetPtr.reset(edgeDocument);
}

bool VertexAccumulators::GraphFormat::buildVertexDocument(arangodb::velocypack::Builder& b,
                                                          const vertex_type* ptr,
                                                          size_t size) const {
  VPackObjectBuilder guard(&b, _resultField);
  for (auto&& acc : ptr->_accumulators) {
    b.add(VPackValue(acc.first));
    acc.second->getValueIntoBuilder(b);
  }
  return true;
}

bool VertexAccumulators::GraphFormat::buildEdgeDocument(arangodb::velocypack::Builder& b,
                                                        const edge_type* ptr,
                                                        size_t size) const {
  // FIXME
  // std::abort();
  return false;
}
