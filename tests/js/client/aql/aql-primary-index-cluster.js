/*jshint globalstrict:false, strict:false, maxlen: 500 */
/*global assertEqual */

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
/// @author Copyright 2012, triAGENS GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

var jsunity = require("jsunity");
var helper = require("@arangodb/aql-helper");
var db = require("@arangodb").db;

////////////////////////////////////////////////////////////////////////////////
/// @brief test suite
////////////////////////////////////////////////////////////////////////////////

function explainSuite () {
  var cn = "UnitTestsAhuacatlPrimary";
  var c;

  var explain = function (query, params) {
    return helper.getCompactPlan(db._createStatement({query: query, bindVars: params, options:  { optimizer: { rules: [ "-all", "+use-indexes" ] } }}).explain()).map(function(node) { return node.type; });
  };

  return {

////////////////////////////////////////////////////////////////////////////////
/// @brief set up
////////////////////////////////////////////////////////////////////////////////

    setUpAll : function () {
      db._drop(cn);
      c = db._create(cn);

      let docs = [];
      for (var i = 0; i < 100; ++i) {
        docs.push({ _key: "testkey" + i, value: i });
      }
      c.insert(docs);
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief tear down
////////////////////////////////////////////////////////////////////////////////

    tearDownAll : function () {
      db._drop(cn);
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test lookup by non-existing _key
////////////////////////////////////////////////////////////////////////////////

    testKeyNotExisting : function () {
      var keys = [ 
        'foobar', 
        'baz', 
        '123', 
        '', 
        ' this key is not valid', 
        ' testkey1', 
        'testkey100', 
        'testkey1000', 
        1, 
        100, 
        false, 
        true, 
        null 
      ];
      var query = "FOR i IN " + cn + " FILTER i._key == @key RETURN i";

      keys.forEach(function(key) {
        assertEqual([ "SingletonNode", "ScatterNode", "RemoteNode", "IndexNode", "RemoteNode", "GatherNode", "CalculationNode", "FilterNode", "ReturnNode" ], explain(query, { key: key }));
        assertEqual([ ], db._query(query, { key: key }).toArray());
      });
    }, 

////////////////////////////////////////////////////////////////////////////////
/// @brief test lookup by existing _key
////////////////////////////////////////////////////////////////////////////////

    testKeyExisting : function () {
      var query = "FOR i IN " + cn + " FILTER i._key == @key RETURN i.value";
      for (var i = 0; i < 100; ++i) {
        assertEqual([ "SingletonNode", "ScatterNode", "RemoteNode", "IndexNode", "RemoteNode", "GatherNode", "CalculationNode", "FilterNode", "CalculationNode", "ReturnNode" ], explain(query, { key: "testkey" + i }));
        assertEqual([ i ], db._query(query, { key: "testkey" + i }).toArray());
      }
    }, 

////////////////////////////////////////////////////////////////////////////////
/// @brief test lookup by non-existing _id
////////////////////////////////////////////////////////////////////////////////

    testIdNotExisting : function () {
      var ids = [ 
        '_users/testkey10',
        ' ' + cn + '/testkey10',
        'othercollection/testkey1',
        'othercollection/testkey10',
        cn + '/testkey100',
        cn + '/testkey101',
        cn + '/testkey1000',
        cn + '/testkey-1',
        'foobar', 
        'baz', 
        '123', 
        '', 
        ' this key is not valid', 
        ' testkey1', 
        'testkey100', 
        'testkey1000', 
        1, 
        100, 
        false, 
        true, 
        null 
      ]; 
      var query = "FOR i IN " + cn + " FILTER i._id == @id RETURN i";

      ids.forEach(function(id) {
        assertEqual([ "SingletonNode", "ScatterNode", "RemoteNode", "IndexNode", "RemoteNode", "GatherNode", "CalculationNode", "FilterNode", "ReturnNode" ], explain(query, { id: id }));
        assertEqual([ ], db._query(query, { id: id }).toArray());
      });
    }, 

////////////////////////////////////////////////////////////////////////////////
/// @brief test lookup by existing _id
////////////////////////////////////////////////////////////////////////////////

    testIdExisting : function () {
      var query = "FOR i IN " + cn + " FILTER i._id == @id RETURN i.value";
      for (var i = 0; i < 100; ++i) {
        assertEqual([ "SingletonNode", "ScatterNode", "RemoteNode", "IndexNode", "RemoteNode", "GatherNode", "CalculationNode", "FilterNode", "CalculationNode", "ReturnNode" ], explain(query, { id: cn + "/testkey" + i }));
        assertEqual([ i ], db._query(query, { id: cn + "/testkey" + i }).toArray());
      }
    },

    testInvalidValuesinList : function () {
      var query = "FOR x IN @idList FOR i IN " + cn + " FILTER i._id == x SORT i.value RETURN i.value";
      var bindParams = {
        idList: [
          null,
          cn + "/testkey1", // Find this
          "blub/bla",
          "noKey",
          cn + "/testkey2", // And this
          123456,
          { "the": "foxx", "is": "wrapped", "in":"objects"},
          [15, "man", "on", "the", "dead", "mans", "chest"],
          cn + "/testkey3" // And this
        ]
      };
      assertEqual([ 1, 2, 3], db._query(query, bindParams).toArray());
    },

    testInvalidValuesInINFilter : function () {
      var query = "FOR i IN " + cn + " FILTER i._id IN @idList SORT i.value RETURN i.value";
      var bindParams = {
        idList: [
          null,
          cn + "/testkey1", // Find this
          "blub/bla",
          "noKey",
          cn + "/testkey2", // And this
          123456,
          { "the": "foxx", "is": "wrapped", "in":"objects"},
          [15, "man", "on", "the", "dead", "mans", "chest"],
          cn + "/testkey3" // And this
        ]
      };
      assertEqual([ 1, 2, 3], db._query(query, bindParams).toArray());
    }
  };
}

////////////////////////////////////////////////////////////////////////////////
/// @brief executes the test suite
////////////////////////////////////////////////////////////////////////////////

jsunity.run(explainSuite);

return jsunity.done();

