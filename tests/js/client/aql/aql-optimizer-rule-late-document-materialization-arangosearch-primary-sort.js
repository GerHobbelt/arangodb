/*jshint globalstrict:false, strict:false, maxlen: 500 */
/*global assertTrue, assertFalse, assertEqual, assertNotEqual */

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
/// @author Yuriy Popov
/// @author Copyright 2019, ArangoDB GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

const jsunity = require("jsunity");
const deriveTestSuite = require('@arangodb/test-helper').deriveTestSuite;
const base = require("fs").join(require('internal').pathForTesting('client'),
             'aql', 'aql-optimizer-rule-late-document-materialization-arangosearch-primary-sort.inc');
const ArangoResearchLateMaterializationTestSuite = require("internal").load(base);
const isCluster = require("internal").isCluster();

function lateDocumentMaterializationArangoSearchPrimarySortRuleTestSuite() {
  let suite = {};
  deriveTestSuite(
    ArangoResearchLateMaterializationTestSuite(false, isCluster, "UnitTestsDB", {}),
    suite,
    "_arangosearch_NonOneShard"
  );
  return suite;
}

function lateDocumentMaterializationSearchAliasPrimarySortRuleTestSuite() {
  let suite = {};
  deriveTestSuite(
    ArangoResearchLateMaterializationTestSuite(true, isCluster, "UnitTestsDB", {}),
    suite,
    "_search-alias_NonOneShard"
  );
  return suite;
}

jsunity.run(lateDocumentMaterializationArangoSearchPrimarySortRuleTestSuite);
jsunity.run(lateDocumentMaterializationSearchAliasPrimarySortRuleTestSuite);

return jsunity.done();
