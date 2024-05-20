/*jshint globalstrict:false, strict:false, maxlen: 500 */
/*global assertEqual, assertNotEqual, assertTrue, assertFalse, assertNull, assertMatch, fail */

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

const internal = require("internal");
const db = require("@arangodb").db;
const jsunity = require("jsunity");
const helper = require("@arangodb/aql-helper");
const getModifyQueryResultsRaw = helper.getModifyQueryResultsRaw;
const assertQueryError = helper.assertQueryError;
const sanitizeStats = helper.sanitizeStats;
const isCluster = require("internal").isCluster();
const disableSingleDocOp = { optimizer : { rules : [ "-optimize-cluster-single-document-operations" ] } };
const disableRestrictToSingleShard = { optimizer : { rules : [ "-restrict-to-single-shard" ] } };

const disableSingleDocOpRestrictToSingleShard = {
  optimizer : {
    rules : [
      "-restrict-to-single-shard",
      "-optimize-cluster-single-document-operations"
    ]
  }
};

let hasDistributeNode = function(nodes) {
  return (nodes.filter(function(node) {
    return node.type === 'DistributeNode';
  }).length > 0);
};

let allNodesOfTypeAreRestrictedToShard = function(nodes, typeName, collection) {
  return nodes.filter(function(node) {
    return node.type === typeName &&
           node.collection === collection.name();
  }).every(function(node) {
    return (collection.shards().indexOf(node.restrictedTo) !== -1);
  });
};

////////////////////////////////////////////////////////////////////////////////
/// @brief test suite
////////////////////////////////////////////////////////////////////////////////

function ahuacatlModifySuite () {
  const errors = internal.errors;
  const cn = "UnitTestsAhuacatlModify";
  const cn2 = "UnitTestsAhuacatlModify2";

  return {

    setUp : function () {
      db._drop(cn);
      db._drop(cn2);
    },

    tearDown : function () {
      db._drop(cn);
      db._drop(cn2);
    },

    // use default shard key (_key)

    testUpdateSingle : function () {
      if (!isCluster) {
        return;
      }
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      { // no - RestrictToSingleShard
        let key = c.insert({ id: "test", value: 1 })._key;

        let expected = { writesExecuted: 1, writesIgnored: 4 };
        let query = "UPDATE { _key: " + JSON.stringify(key) + ", id: 'test' } WITH { value: 2 } IN " + cn;
        let actual = getModifyQueryResultsRaw(query, {}, disableSingleDocOpRestrictToSingleShard);

        let plan = db._createStatement({query: query, bindVars:  {}, options:  disableSingleDocOpRestrictToSingleShard}).explain().plan;

        //assertTrue(hasDistributeNode(plan.nodes)); // the distribute node is not required here
        assertFalse(allNodesOfTypeAreRestrictedToShard(plan.nodes, 'UpdateNode', c));
        assertEqual(-1, plan.rules.indexOf("restrict-to-single-shard"));

        assertEqual(1, c.count());
        assertEqual(0, actual.toArray().length);
        assertEqual(expected, sanitizeStats(actual.getExtra().stats));
        c.truncate({ compact: false });
      }

      // RestrictToSingleShard
      let key = c.insert({ id: "test", value: 1 })._key;

      let expected = { writesExecuted: 1, writesIgnored: 0 };
      let query = "UPDATE { _key: " + JSON.stringify(key) + ", id: 'test' } WITH { value: 2 } IN " + cn;
      let actual = getModifyQueryResultsRaw(query, {}, disableSingleDocOp);

      let plan = db._createStatement({query: query, bindVars:  {}, options:  disableSingleDocOp}).explain().plan;

      assertFalse(hasDistributeNode(plan.nodes));
      assertTrue(allNodesOfTypeAreRestrictedToShard(plan.nodes, 'UpdateNode', c));
      assertNotEqual(-1, plan.rules.indexOf("restrict-to-single-shard"));

      assertEqual(1, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testUpdateSingleShardKeyChange : function () {
      if (!isCluster) {
        return;
      }

      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});
      let key = c.insert({ id: "test", value: 1 })._key;

      assertQueryError(errors.ERROR_CLUSTER_MUST_NOT_CHANGE_SHARDING_ATTRIBUTES.code, "UPDATE { _key: " + JSON.stringify(key) + ", id: 'test' } WITH { value: 2, id: 'bark' } IN " + cn);
    },

    testReplaceSingle : function () {
      if (!isCluster) {
        return;
      }
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});
      { // no - RestrictToSingleShard
        let key = c.insert({ id: "test", value: 1 })._key;

        let expected = { writesExecuted: 1, writesIgnored: 0 };
        let query = "REPLACE { _key: " + JSON.stringify(key) + ", id: 'test' } WITH { id: 'test', value: 2 } IN " + cn;
        let actual = getModifyQueryResultsRaw(query, {}, disableSingleDocOpRestrictToSingleShard);

        let plan = db._createStatement({query: query, bindVars:  {}, options:  disableSingleDocOpRestrictToSingleShard}).explain().plan;
        assertTrue(hasDistributeNode(plan.nodes));
        assertFalse(allNodesOfTypeAreRestrictedToShard(plan.nodes, 'ReplaceNode', c));
        assertEqual(-1, plan.rules.indexOf("restrict-to-single-shard"));
        c.truncate({ compact: false });
      }

      // RestrictToSingleShard
      let key = c.insert({ id: "test", value: 1 })._key;

      let expected = { writesExecuted: 1, writesIgnored: 0 };
      let query = "REPLACE { _key: " + JSON.stringify(key) + ", id: 'test' } WITH { id: 'test', value: 2 } IN " + cn;
      let actual = getModifyQueryResultsRaw(query, {}, disableSingleDocOp);

      let plan = db._createStatement({query: query, bindVars:  {}, options:  disableSingleDocOp}).explain().plan;
      assertFalse(hasDistributeNode(plan.nodes));
      assertTrue(allNodesOfTypeAreRestrictedToShard(plan.nodes, 'ReplaceNode', c));
      assertNotEqual(-1, plan.rules.indexOf("restrict-to-single-shard"));

      assertEqual(1, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testReplaceSingleShardKeyChange : function () {
      if (!isCluster) {
        return;
      }

      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});
      let key = c.insert({ id: "test", value: 1 })._key;

      assertQueryError(errors.ERROR_CLUSTER_MUST_NOT_CHANGE_SHARDING_ATTRIBUTES.code, "REPLACE { _key: " + JSON.stringify(key) + ", id: 'test' } WITH { value: 2, id: 'bark' } IN " + cn);
    },

    testInsertMainLevelWithCustomShardKeyConstant : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      // no - RestrictToSingleShard
      for (let i = 0; i < 30; ++i) {
        let expected = { writesExecuted: 1, writesIgnored: 0 };
        let query = "INSERT { value: " + i + ", id: 'test" + i + "' } IN " + cn;
        let actual = getModifyQueryResultsRaw(query, {}, disableSingleDocOpRestrictToSingleShard);

        if (isCluster) {
          let plan = db._createStatement({query: query, bindVars:  {}, options:  disableSingleDocOpRestrictToSingleShard}).explain().plan;
          assertTrue(hasDistributeNode(plan.nodes));
          assertEqual(-1, plan.rules.indexOf("restrict-to-single-shard"));
        }

        assertEqual(0, actual.toArray().length);
        assertEqual(expected, sanitizeStats(actual.getExtra().stats));
      }
      assertEqual(30, c.count());
      c.truncate({ compact: false });

      // RestrictToSingleShard
      for (let i = 0; i < 30; ++i) {
        let expected = { writesExecuted: 1, writesIgnored: 0 };
        let query = "INSERT { value: " + i + ", id: 'test" + i + "' } IN " + cn;
        let actual = getModifyQueryResultsRaw(query, {}, disableSingleDocOp);

        if (isCluster) {
          let plan = db._createStatement({query: query, bindVars: {}, options:  disableSingleDocOp}).explain().plan;
          assertTrue(hasDistributeNode(plan.nodes));
          assertEqual(-1, plan.rules.indexOf("restrict-to-single-shard"));
        }

        assertEqual(0, actual.toArray().length);
        assertEqual(expected, sanitizeStats(actual.getExtra().stats));
      }
      assertEqual(30, c.count());

      for (let i = 0; i < 30; ++i) {
        let r = db._query("FOR doc IN " + cn + " FILTER doc.id == 'test" + i + "' RETURN doc").toArray();
        assertEqual(1, r.length);
        assertEqual("test" + i, r[0].id);
      }
    },

    testInsertMainLevelWithCustomShardKeyMultiLevel : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["a.b"]});

      // no - RestrictToSingleShard
      for (let i = 0; i < 30; ++i) {
        let expected = { writesExecuted: 1, writesIgnored: 0 };
        let query = "INSERT { value: " + i + ", a: { b: 'test" + i + "' } } IN " + cn;
        let actual = getModifyQueryResultsRaw(query, {}, disableSingleDocOp);

        if (isCluster) {
          let plan = db._createStatement({query: query, bindVars: {}, options:  disableSingleDocOp}).explain().plan;
          assertTrue(hasDistributeNode(plan.nodes));
          assertEqual(-1, plan.rules.indexOf("restrict-to-single-shard"));
        }

        assertEqual(0, actual.toArray().length);
        assertEqual(expected, sanitizeStats(actual.getExtra().stats));
      }
      assertEqual(30, c.count());
      c.truncate({ compact: false });

      // RestrictToSingleShard
      for (let i = 0; i < 30; ++i) {
        let expected = { writesExecuted: 1, writesIgnored: 0 };
        let query = "INSERT { value: " + i + ", a: { b: 'test" + i + "' } } IN " + cn;
        let actual = getModifyQueryResultsRaw(query, {}, disableSingleDocOp);

        if (isCluster) {
          let plan = db._createStatement({query: query, bindVars:  {}, options:  disableSingleDocOp}).explain().plan;
          assertTrue(hasDistributeNode(plan.nodes));
          assertEqual(-1, plan.rules.indexOf("restrict-to-single-shard"));
        }

        assertEqual(0, actual.toArray().length);
        assertEqual(expected, sanitizeStats(actual.getExtra().stats));
      }
      assertEqual(30, c.count());

      for (let i = 0; i < 30; ++i) {
        let r = db._query("FOR doc IN " + cn + " FILTER doc.a.b == 'test" + i + "' RETURN doc").toArray();
        assertEqual(1, r.length);
        assertEqual("test" + i, r[0].a.b);
      }
    },

    testInsertMainLevelWithKeyConstant : function () {
      let c = db._create(cn, {numberOfShards:5});
      // no - RestrictToSingleShard
      for (let i = 0; i < 30; ++i) {
        let expected = { writesExecuted: 1, writesIgnored: 0 };
        let query = "INSERT { value: " + i + ", _key: 'test" + i + "' } IN " + cn;
        let actual = getModifyQueryResultsRaw(query, {}, disableSingleDocOpRestrictToSingleShard);

        if (isCluster) {
          let plan = db._createStatement({query: query, bindVars:  {}, options:  disableSingleDocOpRestrictToSingleShard}).explain().plan;
          assertTrue(hasDistributeNode(plan.nodes));
          assertEqual(-1, plan.rules.indexOf("restrict-to-single-shard"));
        }

        assertEqual(0, actual.toArray().length);
        assertEqual(expected, sanitizeStats(actual.getExtra().stats));
      }
      assertEqual(30, c.count());

      for (let i = 0; i < 30; ++i) {
        let r = db._query("FOR doc IN " + cn + " FILTER doc._key == 'test" + i + "' RETURN doc").toArray();
        assertEqual(1, r.length);
        assertEqual("test" + i, r[0]._key);
        assertEqual(cn + "/test" + i, r[0]._id);

        r = db._query("FOR doc IN " + cn + " FILTER doc._id == '" + cn + "/test" + i + "' RETURN doc").toArray();
        assertEqual(1, r.length);
        assertEqual("test" + i, r[0]._key);
        assertEqual(cn + "/test" + i, r[0]._id);
      }
      c.truncate({ compact: false });

      // RestrictToSingleShard
      for (let i = 0; i < 30; ++i) {
        let expected = { writesExecuted: 1, writesIgnored: 0 };
        let query = "INSERT { value: " + i + ", _key: 'test" + i + "' } IN " + cn;
        let actual = getModifyQueryResultsRaw(query, {}, disableSingleDocOp);

        if (isCluster) {
          let plan = db._createStatement({query: query, bindVars: {}, options:  disableSingleDocOp}).explain().plan;
          assertFalse(hasDistributeNode(plan.nodes));
          assertNotEqual(-1, plan.rules.indexOf("restrict-to-single-shard"));
        }

        assertEqual(0, actual.toArray().length);
        assertEqual(expected, sanitizeStats(actual.getExtra().stats));
      }
      assertEqual(30, c.count());

      for (let i = 0; i < 30; ++i) {
        let r = db._query("FOR doc IN " + cn + " FILTER doc._key == 'test" + i + "' RETURN doc").toArray();
        assertEqual(1, r.length);
        assertEqual("test" + i, r[0]._key);
        assertEqual(cn + "/test" + i, r[0]._id);

        r = db._query("FOR doc IN " + cn + " FILTER doc._id == '" + cn + "/test" + i + "' RETURN doc").toArray();
        assertEqual(1, r.length);
        assertEqual("test" + i, r[0]._key);
        assertEqual(cn + "/test" + i, r[0]._id);
      }
    },

    testInsertMainLevelWithKeyExpression : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 30; ++i) {
        let expected = { writesExecuted: 1, writesIgnored: 0 };
        let query = "INSERT { value: " + i + ", _key: NOOPT(CONCAT('test', '" + i + "')) } IN " + cn;
        let actual = getModifyQueryResultsRaw(query, {}, disableSingleDocOp);

        if (isCluster) {
          let plan = db._createStatement({query: query, bindVars: {}, options:  disableSingleDocOp}).explain().plan;
          assertTrue(hasDistributeNode(plan.nodes));
          assertEqual(-1, plan.rules.indexOf("restrict-to-single-shard"));
        }

        assertEqual(0, actual.toArray().length);
        assertEqual(expected, sanitizeStats(actual.getExtra().stats));
      }
      assertEqual(30, c.count());

      for (let i = 0; i < 30; ++i) {
        let r = db._query("FOR doc IN " + cn + " FILTER doc._key == 'test" + i + "' RETURN doc").toArray();
        assertEqual(1, r.length);
        assertEqual("test" + i, r[0]._key);
        assertEqual(cn + "/test" + i, r[0]._id);

        r = db._query("FOR doc IN " + cn + " FILTER doc._id == '" + cn + "/test" + i + "' RETURN doc").toArray();
        assertEqual(1, r.length);
        assertEqual("test" + i, r[0]._key);
        assertEqual(cn + "/test" + i, r[0]._id);
      }
    },

    testInsertMainLevelWithKey : function () {
      let c = db._create(cn, {numberOfShards:5});

      let expected = { writesExecuted: 2000, writesIgnored: 0 };
      let query = "FOR i IN 1..2000 INSERT { value: i, _key: CONCAT('test', i) } IN " + cn;
      { // no - RestrictToSingleShard
        let actual = getModifyQueryResultsRaw(query);

        if (isCluster) {
          let nodes = db._createStatement({query: query, bindVars:  {}, options:  disableRestrictToSingleShard}).explain().plan.nodes;
          assertTrue(hasDistributeNode(nodes));
        }

        assertEqual(2000, c.count());
        assertEqual(0, actual.toArray().length);
        assertEqual(expected, sanitizeStats(actual.getExtra().stats));
        c.truncate({ compact: false });
      }
      // RestrictToSingleShard
      let actual = getModifyQueryResultsRaw(query);

      if (isCluster) {
        let nodes = db._createStatement(query).explain().plan.nodes;
        assertTrue(hasDistributeNode(nodes));
      }

      assertEqual(2000, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testInsertMainLevelWithKeyReturnNew : function () {
      let c = db._create(cn, {numberOfShards:5});

      let expected = { writesExecuted: 2000, writesIgnored: 0 };
      let query = "FOR i IN 1..2000 INSERT { value: i, _key: CONCAT('test', i) } IN " + cn +  " RETURN NEW";
      let actual = getModifyQueryResultsRaw(query);

      if (isCluster) {
        let nodes = db._createStatement(query).explain().plan.nodes;
        assertTrue(hasDistributeNode(nodes));
      }

      assertEqual(2000, c.count());
      assertEqual(2000, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testInsertMainLevelCustomShardKey : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      let expected = { writesExecuted: 2000, writesIgnored: 0 };
      let query = "FOR i IN 1..2000 INSERT { value: i, id: i } IN " + cn;
      let actual = getModifyQueryResultsRaw(query);

      if (isCluster) {
        let nodes = db._createStatement(query).explain().plan.nodes;
        assertTrue(hasDistributeNode(nodes));
      }

      assertEqual(2000, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testInsertMainLevelCustomShardKeyReturnNew : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      let expected = { writesExecuted: 2000, writesIgnored: 0 };
      let query = "FOR i IN 1..2000 INSERT { value: i, id: i } IN " + cn + " RETURN NEW";
      let actual = getModifyQueryResultsRaw(query);

      if (isCluster) {
        let nodes = db._createStatement(query).explain().plan.nodes;
        assertTrue(hasDistributeNode(nodes));
      }

      assertEqual(2000, c.count());
      assertEqual(2000, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testInsertMainLevel : function () {
      let c = db._create(cn, {numberOfShards:5});

      let expected = { writesExecuted: 2000, writesIgnored: 0 };
      let query = "FOR i IN 1..2000 INSERT { value: i } IN " + cn;
      let actual = getModifyQueryResultsRaw(query);

      if (isCluster) {
        let nodes = db._createStatement(query).explain().plan.nodes;
        assertTrue(hasDistributeNode(nodes));
      }

      assertEqual(2000, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testInsertMainLevelWithReturn : function () {
      let c = db._create(cn, {numberOfShards:5});

      let expected = { writesExecuted: 2000, writesIgnored: 0 };
      let query = "FOR i IN 1..2000 INSERT { value: i } IN " + cn + " RETURN NEW";
      let actual = getModifyQueryResultsRaw(query);

      if (isCluster) {
        let nodes = db._createStatement(query).explain().plan.nodes;
        assertTrue(hasDistributeNode(nodes));
      }

      assertEqual(2000, c.count());
      assertEqual(2000, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testInsertInSubquery : function () {
      let c = db._create(cn, {numberOfShards:5});

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("RETURN (FOR i IN 1..100 INSERT { value: i } IN " + cn + ")");

      assertEqual(100, c.count());
      let res = actual.toArray();
      assertEqual(1, res.length);
      assertEqual([ [ ] ], res);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testInsertInSubqueryWithReturn : function () {
      let c = db._create(cn, {numberOfShards:5});

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("RETURN (FOR i IN 1..100 INSERT { value: i } IN " + cn + " RETURN NEW)");

      assertEqual(100, c.count());
      let res = actual.toArray();
      assertEqual(1, res.length);
      assertEqual(100, res[0].length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveMainLevelWithKeySingle : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ _key: "test" + i });
      }

      let expected = { writesExecuted: 1, writesIgnored: 0 };
      let query = "FOR d IN " + cn + " FILTER d._key == 'test93' REMOVE d IN " + cn;
      let actual = db._query(query, {}, disableSingleDocOp);
      if (isCluster) {
        let plan = db._createStatement({query: query, bindVars:  {}, options:  disableSingleDocOp}).explain().plan;
        assertFalse(hasDistributeNode(plan.nodes));
        assertTrue(allNodesOfTypeAreRestrictedToShard(plan.nodes, 'IndexNode', c));
        assertNotEqual(-1, plan.rules.indexOf("restrict-to-single-shard"));
      }

      assertEqual(99, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveMainLevelWithKeySingleReturnOld : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ _key: "test" + i });
      }

      let expected = { writesExecuted: 1, writesIgnored: 0 };
      let query = "FOR d IN " + cn + " FILTER d._key == 'test93' REMOVE d IN " + cn + " RETURN OLD";
      let actual = db._query(query, {}, disableSingleDocOp);
      if (isCluster) {
        let plan = db._createStatement({query: query, bindVars: {}, options:  disableSingleDocOp}).explain().plan;
        assertFalse(hasDistributeNode(plan.nodes));
        assertTrue(allNodesOfTypeAreRestrictedToShard(plan.nodes, 'IndexNode', c));
        assertNotEqual(-1, plan.rules.indexOf("restrict-to-single-shard"));
      }

      assertEqual(99, c.count());
      assertEqual(1, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveMainLevelWithoutKey : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ _key: "test" + i });
      }

      let query = "FOR d IN " + cn + " REMOVE { foo: 'bar' } IN " + cn;

      assertQueryError(errors.ERROR_ARANGO_DOCUMENT_KEY_MISSING.code, query);
      if (isCluster) {
        let plan = db._createStatement(query).explain().plan;
        assertEqual(-1, plan.rules.indexOf("undistribute-remove-after-enum-coll"));
      }

      assertEqual(100, c.count());
    },

    testRemoveMainLevelSingleKey : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ _key: "test" + i });
      }

      let query = "FOR d IN " + cn + " REMOVE { _key: 'bar' } IN " + cn;

      assertQueryError(errors.ERROR_ARANGO_DOCUMENT_NOT_FOUND.code, query);

      if (isCluster) {
        let plan = db._createStatement(query).explain().plan;
        assertEqual(-1, plan.rules.indexOf("undistribute-remove-after-enum-coll"));
      }

      assertEqual(100, c.count());
    },

    testRemoveMainLevelWithKey : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ _key: "test" + i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let query = "FOR d IN " + cn + " REMOVE { _key: d._key } IN " + cn;
      let actual = getModifyQueryResultsRaw(query);
      if (isCluster) {
        let plan = db._createStatement(query).explain().plan;
        assertFalse(hasDistributeNode(plan.nodes));
        assertNotEqual(-1, plan.rules.indexOf("undistribute-remove-after-enum-coll"));
      }

      assertEqual(0, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveMainLevelWithKeyReturnOld : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ _key: "test" + i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let query = "FOR d IN " + cn + " REMOVE { _key: d._key } IN " + cn + " RETURN OLD";
      let actual = getModifyQueryResultsRaw(query);
      if (isCluster) {
        let plan = db._createStatement(query).explain().plan;
        assertFalse(hasDistributeNode(plan.nodes));
        assertNotEqual(-1, plan.rules.indexOf("undistribute-remove-after-enum-coll"));
      }

      assertEqual(0, c.count());
      assertEqual(100, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveMainLevelWithKey2 : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ _key: "test" + i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let query = "FOR i IN 0..99 REMOVE { _key: CONCAT('test', i) } IN " + cn;
      let actual = getModifyQueryResultsRaw(query);
      if (isCluster) {
        let nodes = db._createStatement(query).explain().plan.nodes;
        assertTrue(hasDistributeNode(nodes));
      }

      assertEqual(0, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveMainLevelWithKey2ReturnOld : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ _key: "test" + i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let query = "FOR i IN 0..99 REMOVE { _key: CONCAT('test', i) } IN " + cn + " RETURN OLD";
      let actual = getModifyQueryResultsRaw(query);
      if (isCluster) {
        let nodes = db._createStatement(query).explain().plan.nodes;
        assertTrue(hasDistributeNode(nodes));
      }

      assertEqual(0, c.count());
      assertEqual(100, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveMainLevelDefaultShardKeyByReference : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id: i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let query = "FOR d IN " + cn + " REMOVE d IN " + cn;
      let actual = getModifyQueryResultsRaw(query);
      if (isCluster) {
        let plan = db._createStatement(query).explain().plan;
        assertFalse(hasDistributeNode(plan.nodes));
        assertNotEqual(-1, plan.rules.indexOf("undistribute-remove-after-enum-coll"));
      }

      assertEqual(0, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveMainLevelDefaultShardKeyByAttribute : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id: i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let query = "FOR d IN " + cn + " REMOVE d._key IN " + cn;
      let actual = getModifyQueryResultsRaw(query);
      if (isCluster) {
        let plan = db._createStatement(query).explain().plan;
        assertFalse(hasDistributeNode(plan.nodes));
        assertNotEqual(-1, plan.rules.indexOf("undistribute-remove-after-enum-coll"));
      }

      assertEqual(0, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveMainLevelDefaultShardKeyByObject : function () {
      let c = db._create(cn, {numberOfShards:5});

      { // no - RestrictToSingleShard
        for (let i = 0; i < 100; ++i) {
          c.insert({ id: i });
        }

        let expected = { writesExecuted: 100, writesIgnored: 0 };
        let query = "FOR d IN " + cn + " REMOVE { _key: d._key } IN " + cn;
        let actual = getModifyQueryResultsRaw(query, {}, disableRestrictToSingleShard);
        if (isCluster) {
          let plan = db._createStatement({query: query, bindVars:  {}, options:  disableRestrictToSingleShard}).explain().plan;
          assertFalse(hasDistributeNode(plan.nodes));
          assertNotEqual(-1, plan.rules.indexOf("undistribute-remove-after-enum-coll"));
        }

        assertEqual(0, c.count());
        assertEqual(0, actual.toArray().length);
        assertEqual(expected, sanitizeStats(actual.getExtra().stats));
      }
      //  RestrictToSingleShard
      for (let i = 0; i < 100; ++i) {
        c.insert({ id: i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let query = "FOR d IN " + cn + " REMOVE { _key: d._key } IN " + cn;
      let actual = getModifyQueryResultsRaw(query);
      if (isCluster) {
        let plan = db._createStatement(query).explain().plan;
        assertFalse(hasDistributeNode(plan.nodes));
        assertNotEqual(-1, plan.rules.indexOf("undistribute-remove-after-enum-coll"));
      }

      assertEqual(0, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveMainLevelCustomShardKeyFixed : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id: 42 });
      }

      let expected;
      let query = "FOR d IN " + cn + " REMOVE { _key: d._key, id: 42 } IN " + cn;
      let actual = getModifyQueryResultsRaw(query);
      if (isCluster) {
        expected = { writesExecuted: 100, writesIgnored: 0 };
        let plan = db._createStatement(query).explain().plan;
        assertFalse(hasDistributeNode(plan.nodes));
        assertEqual(-1, plan.rules.indexOf("undistribute-remove-after-enum-coll"));
        assertNotEqual(-1, plan.rules.indexOf("restrict-to-single-shard"));
      } else {
        expected = { writesExecuted: 100, writesIgnored: 0 };
      }

      assertEqual(0, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    /* Temporarily disabled needs some work on transaction
    testRemoveMainLevelCustomShardKeyFixedSingle : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id: i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      // this will only go to a single shard, as the shardkey is given in REMOVE!
      let query = "FOR d IN " + cn + " REMOVE { _key: d._key, id: 42 } IN " + cn;
      let actual = getModifyQueryResultsRaw(query);
      if (isCluster) {
        let plan = db._createStatement(query).explain().plan;
        assertFalse(hasDistributeNode(plan.nodes));
        assertEqual(-1, plan.rules.indexOf("undistribute-remove-after-enum-coll"));
        assertNotEqual(-1, plan.rules.indexOf("restrict-to-single-shard"));
      }

      assertEqual(0, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },
    */

    testRemoveMainLevelCustomShardKeyFixedSingleWithFilter : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id: i });
      }

      let expected = { writesExecuted: 1, writesIgnored: 0 };
      let query = "FOR d IN " + cn + " FILTER d.id == 42 REMOVE { _key: d._key, id: 42 } IN " + cn;
      let actual = getModifyQueryResultsRaw(query);

      if (isCluster) {
        expected = { writesExecuted: 1, writesIgnored: 0 };
        let plan = db._createStatement(query).explain().plan;
        assertFalse(hasDistributeNode(plan.nodes));
        assertEqual(-1, plan.rules.indexOf("undistribute-remove-after-enum-coll"));
        assertNotEqual(-1, plan.rules.indexOf("restrict-to-single-shard"));
      }

      assertEqual(99, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveMainLevelCustomShardKeys : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id1", "id2"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id1: i, id2: i % 10 });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let query = "FOR d IN " + cn + " REMOVE { _key: d._key, id1: d.id1, id2: d.id2 } IN " + cn;
      let actual = getModifyQueryResultsRaw(query);
      if (isCluster) {
        let plan = db._createStatement(query).explain().plan;
        assertFalse(hasDistributeNode(plan.nodes));
        assertNotEqual(-1, plan.rules.indexOf("undistribute-remove-after-enum-coll"));
      }

      assertEqual(0, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveMainLevelCustomShardKeysFixed : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id1", "id2"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id1: i, id2: i % 10 });
      }

      let expected = { writesExecuted: 100, writesIgnored: isCluster ? 400 : 0 };
      // this will delete all documents!
      let query = "FOR d IN " + cn + " REMOVE { _key: d._key, id1: d.id1, id2: 2 } IN " + cn;
      let actual = getModifyQueryResultsRaw(query);
      if (isCluster) {
        let plan = db._createStatement(query).explain().plan;
        assertFalse(hasDistributeNode(plan.nodes));
        assertEqual(-1, plan.rules.indexOf("undistribute-remove-after-enum-coll"));
        assertEqual(-1, plan.rules.indexOf("restrict-to-single-shard"));
      }

      // this will actually delete all the documents
      assertEqual(0, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveMainLevelCustomShardKeysFixedWithFilter : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id1", "id2"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id1: i, id2: i % 10 });
      }

      let expected = { writesExecuted: 10, writesIgnored: isCluster ? 40 : 0 };
      let query = "FOR d IN " + cn + " FILTER d.id2 == 2 REMOVE { _key: d._key, id1: d.id1, id2: 2 } IN " + cn;
      let actual = getModifyQueryResultsRaw(query);
      if (isCluster) {
        let plan = db._createStatement(query).explain().plan;
        assertFalse(hasDistributeNode(plan.nodes));
        assertEqual(-1, plan.rules.indexOf("undistribute-remove-after-enum-coll"));
      }

      assertEqual(90, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveMainLevelCustomShardKeysWithFilterIndexed : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id1", "id2"]});
      c.ensureIndex({ type: "hash", fields: ["id1", "id2"] });

      for (let i = 0; i < 100; ++i) {
        c.insert({ id1: i, id2: i % 10 });
      }

      let expected = { writesExecuted: 3, writesIgnored: 0 };
      let query = "FOR d IN " + cn + " FILTER d.id1 IN [ 2, 12, 22 ] && d.id2 == 2 REMOVE { _key: d._key, id1: d.id1, id2: d.id2 } IN " + cn;
      let actual = getModifyQueryResultsRaw(query);
      if (isCluster) {
        let plan = db._createStatement(query).explain().plan;
        assertFalse(hasDistributeNode(plan.nodes));
        assertNotEqual(-1, plan.rules.indexOf("undistribute-remove-after-enum-coll"));
      }

      assertEqual(97, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveMainLevelCustomShardKeysMissing : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id1", "id2"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id1: i, id2: i % 10 });
      }

      let expected = { writesExecuted: 100, writesIgnored: isCluster ? 400 : 0 };
      let query = "FOR d IN " + cn + " REMOVE { _key: d._key, id1: d.id1 } IN " + cn;
      let actual = getModifyQueryResultsRaw(query);
      if (isCluster) {
        let plan = db._createStatement(query).explain().plan;
        assertFalse(hasDistributeNode(plan.nodes));
        assertEqual(-1, plan.rules.indexOf("undistribute-remove-after-enum-coll"));
        assertEqual(-1, plan.rules.indexOf("restrict-to-single-shard"));
      }

      assertEqual(0, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveMainLevelCustomShardKey : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id: i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let query = "FOR d IN " + cn + " REMOVE { _key: d._key, id: d.id } IN " + cn;
      let actual = getModifyQueryResultsRaw(query);
      if (isCluster) {
        let plan = db._createStatement(query).explain().plan;
        assertFalse(hasDistributeNode(plan.nodes));
        assertNotEqual(-1, plan.rules.indexOf("undistribute-remove-after-enum-coll"));
      }

      assertEqual(0, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveMainLevelCustomShardKey2 : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id: i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let query = "FOR d IN " + cn + " REMOVE d IN " + cn;
      let actual = getModifyQueryResultsRaw(query);
      if (isCluster) {
        let nodes = db._createStatement(query).explain().plan.nodes;
        assertFalse(hasDistributeNode(nodes));
      }

      assertEqual(0, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveMainLevelCustomShardKeyReturnOld : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id: i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let query = "FOR d IN " + cn + " REMOVE { _key: d._key, id: d.id } IN " + cn + " RETURN OLD";
      let actual = getModifyQueryResultsRaw(query);
      if (isCluster) {
        let nodes = db._createStatement(query).explain().plan.nodes;
        assertFalse(hasDistributeNode(nodes));
      }

      assertEqual(0, c.count());
      assertEqual(100, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveMainLevel : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ _key: "test" + i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let query = "FOR d IN " + cn + " REMOVE d IN " + cn;
      let actual = getModifyQueryResultsRaw(query);
      if (isCluster) {
        let nodes = db._createStatement(query).explain().plan.nodes;
        assertFalse(hasDistributeNode(nodes));
      }

      assertEqual(0, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveMainLevelWithReturn : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ _key: "test" + i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let query = "FOR d IN " + cn + " REMOVE d IN " + cn + " RETURN OLD";
      let actual = getModifyQueryResultsRaw(query);
      if (isCluster) {
        let nodes = db._createStatement(query).explain().plan.nodes;
        assertFalse(hasDistributeNode(nodes));
      }

      assertEqual(0, c.count());
      assertEqual(100, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveInSubquery : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ _key: "test" + i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("RETURN (FOR d IN " + cn + " REMOVE d IN " + cn + ")");

      assertEqual(0, c.count());
      let res = actual.toArray();
      assertEqual(1, res.length);
      assertEqual([ [ ] ], res);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveInSubqueryWithReturn : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ _key: "test" + i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("RETURN (FOR d IN " + cn + " REMOVE d IN " + cn + " RETURN OLD)");

      assertEqual(0, c.count());
      let res = actual.toArray();
      assertEqual(1, res.length);
      assertEqual(100, res[0].length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testUpdateMainLevel : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ _key: "test" + i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let query = "FOR d IN " + cn + " UPDATE d WITH { value: 2 } IN " + cn;
      let actual = getModifyQueryResultsRaw(query);
      if (isCluster) {
        let nodes = db._createStatement(query).explain().plan.nodes;
        assertFalse(hasDistributeNode(nodes));
      }

      assertEqual(100, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testUpdateMainLevelWithReturnOld : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ _key: "test" + i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let query = "FOR d IN " + cn + " UPDATE d WITH { value: 2 } IN " + cn + " RETURN OLD";
      let actual = getModifyQueryResultsRaw(query);
      if (isCluster) {
        let nodes = db._createStatement(query).explain().plan.nodes;
        assertFalse(hasDistributeNode(nodes));
      }

      assertEqual(100, c.count());
      assertEqual(100, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testUpdateMainLevelWithReturnNew : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ _key: "test" + i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("FOR d IN " + cn + " UPDATE d WITH { value: 2 } IN " + cn + " RETURN NEW");

      assertEqual(100, c.count());
      assertEqual(100, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testUpdateInSubquery : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ _key: "test" + i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("RETURN (FOR d IN " + cn + " UPDATE d WITH { value: 2 } IN " + cn + ")");

      assertEqual(100, c.count());
      let res = actual.toArray();
      assertEqual(1, res.length);
      assertEqual([ [ ] ], res);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testUpdateInSubqueryWithReturnOld : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ _key: "test" + i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("RETURN (FOR d IN " + cn + " UPDATE d WITH { value: 2 } IN " + cn + " RETURN OLD)");

      assertEqual(100, c.count());
      let res = actual.toArray();
      assertEqual(1, res.length);
      assertEqual(100, res[0].length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testUpdateInSubqueryWithReturnNew : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ _key: "test" + i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("RETURN (FOR d IN " + cn + " UPDATE d WITH { value: 2 } IN " + cn + " RETURN NEW)");

      assertEqual(100, c.count());
      let res = actual.toArray();
      assertEqual(1, res.length);
      assertEqual(100, res[0].length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testReplaceMainLevel : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ _key: "test" + i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("FOR d IN " + cn + " REPLACE d WITH { value: 2 } IN " + cn);

      assertEqual(100, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testReplaceMainLevelWithReturnOld : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ _key: "test" + i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("FOR d IN " + cn + " REPLACE d WITH { value: 2 } IN " + cn + " RETURN OLD");

      assertEqual(100, c.count());
      assertEqual(100, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testReplaceMainLevelWithReturnNew : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ _key: "test" + i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("FOR d IN " + cn + " REPLACE d WITH { value: 2 } IN " + cn + " RETURN NEW");

      assertEqual(100, c.count());
      assertEqual(100, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testReplaceInSubquery : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ _key: "test" + i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("RETURN (FOR d IN " + cn + " REPLACE d WITH { value: 2 } IN " + cn + ")");

      assertEqual(100, c.count());
      let res = actual.toArray();
      assertEqual(1, res.length);
      assertEqual([ [ ] ], res);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testReplaceInSubqueryWithReturnOld : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ _key: "test" + i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("RETURN (FOR d IN " + cn + " REPLACE d WITH { value: 2 } IN " + cn + " RETURN OLD)");

      assertEqual(100, c.count());
      let res = actual.toArray();
      assertEqual(1, res.length);
      assertEqual(100, res[0].length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testReplaceInSubqueryWithReturnNew : function () {
      let c = db._create(cn, {numberOfShards:5});

      for (let i = 0; i < 100; ++i) {
        c.insert({ _key: "test" + i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("RETURN (FOR d IN " + cn + " REPLACE d WITH { value: 2 } IN " + cn + " RETURN NEW)");

      assertEqual(100, c.count());
      let res = actual.toArray();
      assertEqual(1, res.length);
      assertEqual(100, res[0].length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    // use custom shard key

    testInsertMainLevelCustomShardKeyWithReturn : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("FOR i IN 1..100 INSERT { id: i, value: i } IN " + cn + " RETURN NEW");

      assertEqual(100, c.count());
      assertEqual(100, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testInsertCustomShardKeyInSubquery : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("RETURN (FOR i IN 1..100 INSERT { id: i, value: i } IN " + cn + ")");

      assertEqual(100, c.count());
      let res = actual.toArray();
      assertEqual(1, res.length);
      assertEqual([ [ ] ], res);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testInsertCustomShardKeyInSubqueryWithReturn : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("RETURN (FOR i IN 1..100 INSERT { id: i, value: i } IN " + cn + " RETURN NEW)");

      assertEqual(100, c.count());
      let res = actual.toArray();
      assertEqual(1, res.length);
      assertEqual(100, res[0].length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveCustomShardKeyMainLevel : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id: i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("FOR d IN " + cn + " REMOVE d IN " + cn);

      assertEqual(0, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveCustomShardKeyMainLevelWithReturn : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id: i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("FOR d IN " + cn + " REMOVE d IN " + cn + " RETURN OLD");

      assertEqual(0, c.count());
      assertEqual(100, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testRemoveCustomShardKeyInSubquery : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      { // no - RestrictToSingleShard
        for (let i = 0; i < 100; ++i) {
          c.insert({ id: i });
        }

        let expected = { writesExecuted: 100, writesIgnored: 0 };
        let actual = getModifyQueryResultsRaw("RETURN (FOR d IN " + cn + " REMOVE d IN " + cn + ")", {}, disableRestrictToSingleShard);

        assertEqual(0, c.count());
      let res = actual.toArray();
      assertEqual(1, res.length);
        assertEqual([ [ ] ], res);
        assertEqual(expected, sanitizeStats(actual.getExtra().stats));
      }

      { // RestrictToSingleShard
        for (let i = 0; i < 100; ++i) {
          c.insert({ id: i });
        }

        let expected = { writesExecuted: 100, writesIgnored: 0 };
        let actual = getModifyQueryResultsRaw("RETURN (FOR d IN " + cn + " REMOVE d IN " + cn + ")");

        assertEqual(0, c.count());
      let res = actual.toArray();
      assertEqual(1, res.length);
        assertEqual([ [ ] ], res);
        assertEqual(expected, sanitizeStats(actual.getExtra().stats));
      }
    },

    testRemoveCustomShardKeyInSubqueryWithReturn : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id: i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("RETURN (FOR d IN " + cn + " REMOVE d IN " + cn + " RETURN OLD)");

      assertEqual(0, c.count());
      let res = actual.toArray();
      assertEqual(1, res.length);
      assertEqual(100, res[0].length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testUpdateCustomShardKeyMainLevel : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id: i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("FOR d IN " + cn + " UPDATE d WITH { value: 2 } IN " + cn);

      assertEqual(100, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testUpdateCustomShardKeyMainLevelWithReturnOld : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id: i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("FOR d IN " + cn + " UPDATE d WITH { value: 2 } IN " + cn + " RETURN OLD");

      assertEqual(100, c.count());
      assertEqual(100, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testUpdateCustomShardKeyMainLevelWithReturnNew : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id: i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("FOR d IN " + cn + " UPDATE d WITH { value: 2 } IN " + cn + " RETURN NEW");

      assertEqual(100, c.count());
      assertEqual(100, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testUpdateCustomShardKeyInSubquery : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id: i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("RETURN (FOR d IN " + cn + " UPDATE d WITH { value: 2 } IN " + cn + ")");

      assertEqual(100, c.count());
      let res = actual.toArray();
      assertEqual(1, res.length);
      assertEqual([ [ ] ], res);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testUpdateCustomShardKeyInSubqueryWithReturnOld : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id: i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("RETURN (FOR d IN " + cn + " UPDATE d WITH { value: 2 } IN " + cn + " RETURN OLD)");

      assertEqual(100, c.count());
      let res = actual.toArray();
      assertEqual(1, res.length);
      assertEqual(100, res[0].length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testUpdateCustomShardKeyInSubqueryWithReturnNew : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id: i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("RETURN (FOR d IN " + cn + " UPDATE d WITH { value: 2 } IN " + cn + " RETURN NEW)");

      assertEqual(100, c.count());
      let res = actual.toArray();
      assertEqual(1, res.length);
      assertEqual(100, res[0].length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testReplaceCustomShardKeyMainLevel : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id: i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("FOR d IN " + cn + " REPLACE d WITH { id: d.id, value: 2 } IN " + cn);

      assertEqual(100, c.count());
      assertEqual(0, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testReplaceCustomShardKeyMainLevelWithReturnOld : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id: i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("FOR d IN " + cn + " REPLACE d WITH { id: d.id, value: 2 } IN " + cn + " RETURN OLD");

      assertEqual(100, c.count());
      assertEqual(100, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testReplaceCustomShardKeyMainLevelWithReturnNew : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id: i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("FOR d IN " + cn + " REPLACE d WITH { id: d.id, value: 2 } IN " + cn + " RETURN NEW");

      assertEqual(100, c.count());
      assertEqual(100, actual.toArray().length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testReplaceCustomShardKeyInSubquery : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id: i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("RETURN (FOR d IN " + cn + " REPLACE d WITH { id: d.id, value: 2 } IN " + cn + ")");

      assertEqual(100, c.count());
      let res = actual.toArray();
      assertEqual(1, res.length);
      assertEqual([ [ ] ], res);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testReplaceCustomShardKeyInSubqueryWithReturnOld : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id: i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("RETURN (FOR d IN " + cn + " REPLACE d WITH { id: d.id, value: 2 } IN " + cn + " RETURN OLD)");

      assertEqual(100, c.count());
      let res = actual.toArray();
      assertEqual(1, res.length);
      assertEqual(100, res[0].length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    testReplaceCustomShardKeyInSubqueryWithReturnNew : function () {
      let c = db._create(cn, {numberOfShards:5, shardKeys: ["id"]});

      for (let i = 0; i < 100; ++i) {
        c.insert({ id: i });
      }

      let expected = { writesExecuted: 100, writesIgnored: 0 };
      let actual = getModifyQueryResultsRaw("RETURN (FOR d IN " + cn + " REPLACE d WITH { id: d.id, value: 2 } IN " + cn + " RETURN NEW)");

      assertEqual(100, c.count());
      let res = actual.toArray();
      assertEqual(1, res.length);
      assertEqual(100, res[0].length);
      assertEqual(expected, sanitizeStats(actual.getExtra().stats));
    },

    // Regression test for a bug in ExecutionPlan::instantiateFromPlan, where
    // the false branch was not part of the plan anymore (at least, not visible
    // from the root) if the condition was `true` at compile time.
    testTernaryEvaluateBothTrue : function () {
      const c1 = db._create(cn);
      const c2 = db._create(cn2);

      const query = `LET x = true ? (INSERT {value: 1} INTO ${cn}) : (INSERT {value: 2} INTO ${cn2}) RETURN x`;
      db._query(query);
      assertEqual([1], c1.toArray().map(o => o.value));
      assertEqual([], c2.toArray().map(o => o.value));
    },

    // Complementary test to testTernaryEvaluateBothTrue, with a constant `false`
    // condition.
    testTernaryEvaluateBothFalse : function () {
      const c1 = db._create(cn);
      const c2 = db._create(cn2);

      const query = `LET x = false ? (INSERT {value: 1} INTO ${cn}) : (INSERT {value: 2} INTO ${cn2}) RETURN x`;
      db._query(query);
      assertEqual([], c1.toArray().map(o => o.value));
      assertEqual([2], c2.toArray().map(o => o.value));
    },

    // Complementary test to testTernaryEvaluateBothTrue, with a non-constant
    // condition.
    testTernaryEvaluateBothRand : function () {
      const c1 = db._create(cn);
      const c2 = db._create(cn2);

      const query = `LET x = RAND() < 0.5 ? (INSERT {value: 1} INTO ${cn}) : (INSERT {value: 2} INTO ${cn2}) RETURN x`;

      for (let i = 0; i < 10; ++i) {
        db._query(query);
        let q1 = c1.toArray().map(o => o.value);
        let q2 = c2.toArray().map(o => o.value);
        assertTrue(q1.length === 1 || q2.length === 1, {q1, q2});
        assertNotEqual(q1.length, q2.length, {q1, q2});
        if (q1.length) {
          assertEqual([1], c1.toArray().map(o => o.value));
          assertEqual([], c2.toArray().map(o => o.value));
        } else {
          assertEqual([], c1.toArray().map(o => o.value));
          assertEqual([2], c2.toArray().map(o => o.value));
        }
        c1.truncate({ compact: false });
        c2.truncate({ compact: false });
      }
    },
  };
}

function ahuacatlModifySkipSuite () {
  var errors = internal.errors;
  var cn = "UnitTestsAhuacatlModify";

  return {

    setUp : function () {
      db._drop(cn);
      let c = db._create(cn, {numberOfShards:5});
      for (let i = 0; i < 1000; ++i) {
        c.insert({ value1 : i, value2: (i % 10), updated: false });
      }
    },

    tearDown : function () {
      db._drop(cn);
    },
    
    testUpdateFull : function () {
      let query = "FOR doc IN " + cn + " UPDATE doc WITH { updated: true } IN " + cn + " COLLECT WITH COUNT INTO l RETURN l";
      let result = db._query(query);
      assertEqual(1000, result.toArray()[0]);

      assertEqual(0, db[cn].byExample({ updated: false }).toArray().length);
      assertEqual(1000, db[cn].byExample({ updated: true }).toArray().length);
    },
    
    testUpdateLimit : function () {
      let query = "FOR doc IN " + cn + " LIMIT 500 UPDATE doc WITH { updated: true } IN " + cn + " COLLECT WITH COUNT INTO l RETURN l";
      let result = db._query(query);
      assertEqual(500, result.toArray()[0]);

      assertEqual(500, db[cn].byExample({ updated: false }).toArray().length);
      assertEqual(500, db[cn].byExample({ updated: true }).toArray().length);
    },

    testUpdateSkip : function () {
      let query = "FOR doc IN " + cn + " LIMIT 100, 200 UPDATE doc WITH { updated: true } IN " + cn + " COLLECT WITH COUNT INTO l RETURN l";
      let result = db._query(query);
      assertEqual(200, result.toArray()[0]);

      assertEqual(800, db[cn].byExample({ updated: false }).toArray().length);
      assertEqual(200, db[cn].byExample({ updated: true }).toArray().length);
    },
    
    testUpdateSkipAlmostAll : function () {
      let query = "FOR doc IN " + cn + " LIMIT 990, 5 UPDATE doc WITH { updated: true } IN " + cn + " COLLECT WITH COUNT INTO l RETURN l";
      let result = db._query(query);
      assertEqual(5, result.toArray()[0]);

      assertEqual(995, db[cn].byExample({ updated: false }).toArray().length);
      assertEqual(5, db[cn].byExample({ updated: true }).toArray().length);
    },
    
    testUpdateSkipAll : function () {
      let query = "FOR doc IN " + cn + " LIMIT 1000, 200 UPDATE doc WITH { updated: true } IN " + cn + " COLLECT WITH COUNT INTO l RETURN l";
      let result = db._query(query);
      assertEqual(0, result.toArray()[0]);

      assertEqual(1000, db[cn].byExample({ updated: false }).toArray().length);
      assertEqual(0, db[cn].byExample({ updated: true }).toArray().length);
    },
    
    testUpdateSkipTheUniverse : function () {
      let query = "FOR doc IN " + cn + " LIMIT 1000000, 1 UPDATE doc WITH { updated: true } IN " + cn + " COLLECT WITH COUNT INTO l RETURN l";
      let result = db._query(query);
      assertEqual(0, result.toArray()[0]);

      assertEqual(1000, db[cn].byExample({ updated: false }).toArray().length);
      assertEqual(0, db[cn].byExample({ updated: true }).toArray().length);
    },
    
    testRemoveFull : function () {
      let query = "FOR doc IN " + cn + " REMOVE doc IN " + cn + " COLLECT WITH COUNT INTO l RETURN l";
      let result = db._query(query);
      assertEqual(1000, result.toArray()[0]);

      assertEqual(0, db[cn].count());
    },
    
    testRemoveLimit : function () {
      let query = "FOR doc IN " + cn + " LIMIT 500 REMOVE doc IN " + cn + " COLLECT WITH COUNT INTO l RETURN l";
      let result = db._query(query);
      assertEqual(500, result.toArray()[0]);

      assertEqual(500, db[cn].count());
    },

    testRemoveSkip : function () {
      let query = "FOR doc IN " + cn + " LIMIT 100, 200 REMOVE doc IN " + cn + " COLLECT WITH COUNT INTO l RETURN l";
      let result = db._query(query);
      assertEqual(200, result.toArray()[0]);

      assertEqual(800, db[cn].count());
    },
    
    testRemoveSkipAlmostAll : function () {
      let query = "FOR doc IN " + cn + " LIMIT 990, 5 REMOVE doc IN " + cn + " COLLECT WITH COUNT INTO l RETURN l";
      let result = db._query(query);
      assertEqual(5, result.toArray()[0]);

      assertEqual(995, db[cn].count());
    },
    
    testRemoveSkipAll : function () {
      let query = "FOR doc IN " + cn + " LIMIT 1000, 200 REMOVE doc IN " + cn + " COLLECT WITH COUNT INTO l RETURN l";
      let result = db._query(query);
      assertEqual(0, result.toArray()[0]);

      assertEqual(1000, db[cn].count());
    },
    
    testRemoveSkipTheUniverse : function () {
      let query = "FOR doc IN " + cn + " LIMIT 1000000, 1 REMOVE doc IN " + cn + " COLLECT WITH COUNT INTO l RETURN l";
      let result = db._query(query);
      assertEqual(0, result.toArray()[0]);

      assertEqual(1000, db[cn].count());
    },

    testSubqueryFullCount : function () {
      const query = "LET sub = NOOPT(FOR doc IN " + cn + " REMOVE doc IN " + cn + " RETURN OLD) COLLECT WITH COUNT INTO l RETURN l"; 
      let result = db._query(query, null, { optimizer: { rules: ["-all"] } });
      let res = result.toArray();
      assertEqual(1, res.length);
      assertEqual(1, res[0]);
    },

  };
}

function ahuacatlGeneratedSuite() {
  var cn = "SubqueryChaosCollection0";
  var cn2 = "SubqueryChaosCollection1";
  var cn3 = "SubqueryChaosCollection2";
  const cleanup = () => {
    try {
      db._drop(cn);
    } catch (e) { }
    try {
      db._drop(cn2);
    } catch (e) { }
    try {
      db._drop(cn3);
    } catch (e) { }
  };

  return {
    setUp: function () {
      cleanup();
      let c = db._create(cn, { numberOfShards: 5 });
      let c1 = db._create(cn2, { numberOfShards: 5 });
      let c2 = db._create(cn3, { numberOfShards: 5 });
      const docs = [];
      for (let i = 1; i < 11; ++i) {
        docs.push({ value: i });
      }
      c.save(docs);
      c1.save(docs);
      c2.save(docs);
    },

    tearDown: function () {
      cleanup();
    },

    testNonSplicedExecutor: function () {
      const q = `
	FOR fv0 IN ${cn}
	  LET sq1 = (FOR fv2 IN ${cn2}
	    UPSERT {value: fv0.value}
	      INSERT {value: 24}
	      UPDATE {updated: true} IN ${cn2}
	    LIMIT 14,5
	    RETURN {fv2: UNSET_RECURSIVE(fv2,"_rev", "_id", "_key")})
	  LIMIT 3,2
	  RETURN {fv0: UNSET_RECURSIVE(fv0,"_rev", "_id", "_key"), sq1: UNSET_RECURSIVE(sq1,"_rev", "_id",  "_key")}`;
      const res = db._query(q);
      assertEqual(100, res.getExtra().stats.writesExecuted);
      assertEqual(2, res.toArray().length);
    },

    testNonSplicedViolatesPassthrough: function () {
      const q = `
        FOR fv0 IN ${cn} 
        LET sq1 = (FOR fv2 IN ${cn2} 
          LIMIT 2,13
          RETURN {fv2: UNSET_RECURSIVE(fv2,"_rev", "_id", "_key")})
        LET sq3 = (FOR fv4 IN ${cn2} 
          UPSERT {value: fv4.value  }  INSERT {value: 71 }  UPDATE {value: 21, updated: true} IN ${cn2}
          LIMIT 11,2
          RETURN {fv4: UNSET_RECURSIVE(fv4,"_rev", "_id", "_key"), sq1: UNSET_RECURSIVE(sq1,"_rev", "_id", 
        "_key")})
        LIMIT 8,3
        RETURN {fv0: UNSET_RECURSIVE(fv0,"_rev", "_id", "_key"), sq1: UNSET_RECURSIVE(sq1,"_rev", "_id", 
        "_key"), sq3: UNSET_RECURSIVE(sq3,"_rev", "_id", "_key")}`;

      const res = db._query(q);
      assertEqual(100, res.getExtra().stats.writesExecuted);
      assertEqual(2, 2, res.toArray().length);
    },

    testSubquerySkipReporting: function () {
      const q = `
        FOR fv0 IN ${cn} 
          LET sq1 = (FOR fv2 IN ${cn2} 
            LET sq3 = (FOR fv4 IN ${cn3} 
              UPSERT {value: fv4.value  }  INSERT {value: fv4.value }  UPDATE { updated: true } IN ${cn3}
              LIMIT 14,4
              RETURN {fv4: UNSET_RECURSIVE(fv4,"_rev", "_id", "_key")})
            LIMIT 0,17
            RETURN {fv2: UNSET_RECURSIVE(fv2,"_rev", "_id", "_key"), sq3: UNSET_RECURSIVE(sq3,"_rev", "_id", "_key")})
          LIMIT 19,11
          COLLECT WITH COUNT INTO counter 
          RETURN {counter: UNSET_RECURSIVE(counter,"_rev", "_id", "_key")}
      `;

      const res = db._query(q);
      assertEqual(1000, res.getExtra().stats.writesExecuted);
      assertEqual(1, res.toArray().length);
    },

    testSubqueryChaos4: function () {
      const q = `
      FOR fv0 IN ${cn} 
        LET sq1 = (FOR fv2 IN ${cn2} 
          LET sq3 = (FOR fv4 IN ${cn3} 
            /* UPSERT { value: fv2.value } INSERT { value: fv2.value } UPDATE {updated: true} IN ${cn3} */
            UPDATE  fv4 WITH {updated: true} IN ${cn3}
            LIMIT 3,12
            RETURN {fv4: UNSET_RECURSIVE(fv4,"_rev", "_id", "_key")})
          FILTER fv0 < 14
          LIMIT 3,19
          RETURN {fv2: UNSET_RECURSIVE(fv2,"_rev", "_id", "_key"), sq3: UNSET_RECURSIVE(sq3,"_rev", "_id", "_key")})
        LIMIT 4,12
        RETURN {fv0: UNSET_RECURSIVE(fv0,"_rev", "_id", "_key"), sq1: UNSET_RECURSIVE(sq1,"_rev", "_id", "_key")}
      `;
      const res = db._query(q);
      assertEqual(1000, res.getExtra().stats.writesExecuted);
      assertEqual(6, res.toArray().length);
    },

    testSkipOverModifySubquery: function() {
      const cn = "UnitTestModifySubquery";
      try {
        db._create(cn);
        const q = `
          FOR i IN 1..2
            LET noModSub = (
              LET modSub = (
                FOR j IN 1..2
                INSERT {} INTO ${cn}
              )
              RETURN modSub
            )
          LIMIT 1,0
          RETURN noModSub`;
        let res = db._query(q);
        assertEqual([], res.toArray());
        assertEqual(db[cn].count(), 4);
      } finally {
        db._drop(cn);
      }
    },
    testChaosGenerated15: function() {
      const q = 
        `FOR fv0 IN 1..10 /* SubqueryChaosCollection0  */
           LET sq1 = (FOR fv2 IN SubqueryChaosCollection1 
             LET sq3 = (FOR fv4 IN SubqueryChaosCollection2
               UPDATE { _key: fv4._key } WITH {updated: true} IN SubqueryChaosCollection2 
               RETURN {fv4})
             LIMIT 5,3
           RETURN {fv2: UNSET_RECURSIVE(fv2,"_rev", "_id", "_key"), sq3})
         RETURN sq1`;

      const res = db._query(q);
      assertEqual(1000, res.getExtra().stats.writesExecuted);
      assertEqual(10, res.toArray().length);
    }
  };
}

jsunity.run(ahuacatlModifySuite);
jsunity.run(ahuacatlModifySkipSuite);
jsunity.run(ahuacatlGeneratedSuite);

return jsunity.done();
