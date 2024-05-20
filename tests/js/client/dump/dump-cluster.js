/*jshint globalstrict:false, strict:false, maxlen : 4000 */

// //////////////////////////////////////////////////////////////////////////////
// / DISCLAIMER
// /
// / Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
// / Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
// /
// / Licensed under the Business Source License 1.1 (the "License");
// / you may not use this file except in compliance with the License.
// / You may obtain a copy of the License at
// /
// /     https://github.com/arangodb/arangodb/blob/devel/LICENSE
// /
// / Unless required by applicable law or agreed to in writing, software
// / distributed under the License is distributed on an "AS IS" BASIS,
// / WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// / See the License for the specific language governing permissions and
// / limitations under the License.
// /
// / Copyright holder is ArangoDB GmbH, Cologne, Germany
// /
/// @author Jan Steemann
/// @author Wilfried Goesgens
/// @author Copyright 2021, ArangoDB GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////


var internal = require("internal");
var jsunity = require("jsunity");
const deriveTestSuite = require('@arangodb/test-helper').deriveTestSuite;
const base = require("fs").join(
  internal.pathForTesting('client'),
  'dump',
  'dump-test.inc');
const baseTests = require("internal").load(base);

jsunity.run(function dump_cluster_testsuite() {

  let suite = {};
  let enterpriseTests = [];
  if (!internal.isEnterprise()) {
    enterpriseTests = [
      "testVertices",
      "testVerticesAqlRead",
      "testVerticesAqlInsert",
      "testOrphans",
      "testOrphansAqlRead",
      "testOrphansAqlInsert",
      "testSatelliteCollections",
      "testSatelliteGraph",
      "testHiddenCollectionsOmitted",
      "testShadowCollectionsOmitted",
      "testEEEdges",
      "testEdgesAqlRead",
      "testEdgesAqlInsert",
      "testAqlGraphQueryOutbound",
      "testAqlGraphQueryAny",
      "testSmartGraphSharding",
      "testViewOnSmartEdgeCollection",
      "testSmartGraphAttribute",
      "testLatestId",
      "testAnalyzers"
    ];
  }
  deriveTestSuite(
    baseTests(),
    suite,
    "_cluster",
    [ // <-- Blacklisted Tests
      // Magic Hint: Those tests are tests which you need to additional blacklist in case they are not supported
      // in that specific environment. Those blacklist is separately and manually maintained per test-suite.
      "testUsers",
      "testTransactionCommit",
      "testTransactionUpdate",
      "testTransactionAbort",

      // enterprise sharded graphs on single server tests
      "testEmptySmartGraph",
      "testEmptyEnterpriseGraph",
      "testEmptySatelliteGraph",
      "testEmptyDisjointGraph",
      "testSmartGraphWithoutData",
      "testEnterpriseGraphWithoutData",
      "testSmartGraphSingleServer",
      "testEnterpriseGraphSingleServer",
      "testSatelliteSmartGraphSingleServer",
      "testDisjointGraphSingleServer",
      "testHybridSmartGraphSingleServer",
      "testHybridDisjointSmartGraphSingleServer"

    ].concat(enterpriseTests)
  );

  return suite;

});

return jsunity.done();
