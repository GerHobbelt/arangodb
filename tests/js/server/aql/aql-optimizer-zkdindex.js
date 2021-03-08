/* global AQL_EXPLAIN, AQL_EXECUTE */
////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2021-2021 ArangoDB GmbH, Cologne, Germany
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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

'use strict';

const jsunity = require("jsunity");
const arangodb = require("@arangodb");
const db = arangodb.db;
const aql = arangodb.aql;
const {assertTrue, assertFalse, assertEqual} = jsunity.jsUnity.assertions;
const _ = require("lodash");

const useIndexes = 'use-indexes';
const removeFilterCoveredByIndex = "remove-filter-covered-by-index";

function optimizerRuleZkd2dIndexTestSuite() {
  const colName = 'UnitTestZkdIndexCollection';
  let col;

  return {
    setUpAll: function () {
      col = db._create(colName);
      col.ensureIndex({type: 'zkd', name: 'zkdIndex', fields: ['x', 'y']});
      // Insert 1001 points
      // (-500, -499.5), (-499.1, -499.4), ..., (0, 0.5), ..., (499.9, 500.4), (500, 500.5)
      db._query(aql`
        FOR i IN 0..1000
          LET x = (i - 500) / 10
          LET y = x + 0.5
          INSERT {x, y, i} INTO ${col}
      `);
    },

    tearDownAll: function () {
      col.drop();
    },

    test1: function () {
      const query = aql`
        FOR d IN ${col}
          FILTER d.i == 0
          RETURN d
      `;
      const res = AQL_EXPLAIN(query.query, query.bindVars);
      const nodeTypes = res.plan.nodes.map(n => n.type);
      const appliedRules = res.plan.rules;
      assertEqual([ "SingletonNode", "EnumerateCollectionNode", "ReturnNode" ], nodeTypes);
      assertFalse(appliedRules.includes(useIndexes));
      assertFalse(appliedRules.includes(removeFilterCoveredByIndex));
    },

    test2: function () {
      const query = aql`
        FOR d IN ${col}
          FILTER 0 <= d.x && d.x <= 1
          RETURN d.x
      `;
      const explainRes = AQL_EXPLAIN(query.query, query.bindVars);
      const appliedRules = explainRes.plan.rules;
      const nodeTypes = explainRes.plan.nodes.map(n => n.type);
      assertEqual([ "SingletonNode", "IndexNode", "CalculationNode", "ReturnNode" ], nodeTypes);
      assertTrue(appliedRules.includes(useIndexes));
      assertTrue(appliedRules.includes(removeFilterCoveredByIndex));
      const executeRes = AQL_EXECUTE(query.query, query.bindVars);
      const res = executeRes.json;
      res.sort();
      assertEqual([ 0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1 ], res);
    },

  };
}

jsunity.run(optimizerRuleZkd2dIndexTestSuite);

return jsunity.done();
