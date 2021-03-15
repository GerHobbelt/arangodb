/*jshint globalstrict:false, strict:false, unused: false */
/*global assertEqual, assertTrue, arango, ARGUMENTS */

////////////////////////////////////////////////////////////////////////////////
/// @brief test the replication
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2010-2012 triagens GmbH, Cologne, Germany
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
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Jan Steemann
/// @author Copyright 2017, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

var jsunity = require("jsunity");
var arangodb = require("@arangodb");
var db = arangodb.db;

const reconnectRetry = require('@arangodb/replication-common').reconnectRetry;
var replication = require("@arangodb/replication");
let compareTicks = replication.compareTicks;
var console = require("console");
var internal = require("internal");
var masterEndpoint = arango.getEndpoint();
const slaveEndpoint = ARGUMENTS[ARGUMENTS.length - 1];

////////////////////////////////////////////////////////////////////////////////
/// @brief test suite
////////////////////////////////////////////////////////////////////////////////

let emit = function(msg) {
  internal.print(msg);
  console.log(msg);
};

function ReplicationSuite() {
  'use strict';
  var cn = "UnitTestsReplication";

  var connectToMaster = function() {
    reconnectRetry(masterEndpoint, db._name(), "root", "");
    db._flushCache();
  };

  var connectToSlave = function() {
    reconnectRetry(slaveEndpoint, db._name(), "root", "");
    db._flushCache();
  };

  var collectionChecksum = function(name) {
    return db._collection(name).checksum(false, true).checksum;
  };

  var collectionCount = function(name) {
    return db._collection(name).count();
  };

  var compare = function(masterFunc, masterFunc2, slaveFuncFinal) {
    var state = {};

    assertEqual(cn, db._name());
    db._flushCache();
    masterFunc(state);
    
    connectToSlave();
    assertEqual(cn, db._name());

    let syncResult = replication.sync({
      endpoint: masterEndpoint,
      username: "root",
      password: "",
      verbose: true,
      includeSystem: false,
      keepBarrier: true,
      requireFromPresent: true,
      incremental: true
    });

    assertTrue(syncResult.hasOwnProperty('lastLogTick'));

    connectToMaster();
    masterFunc2(state);

    // use lastLogTick as of now
    state.lastLogTick = replication.logger.state().state.lastUncommittedLogTick;

    let applierConfiguration = {
      endpoint: masterEndpoint,
      username: "root",
      password: "",
      requireFromPresent: true,
      autoResync: true,
      autoResyncRetries: 5 
    };

    connectToSlave();
    assertEqual(cn, db._name());

    replication.applier.properties(applierConfiguration);
    replication.applier.start(syncResult.lastLogTick, syncResult.barrierId);

    var printed = false;

    while (true) {
      var slaveState = replication.applier.state();

      if (slaveState.state.lastError.errorNum > 0) {
        console.topic("replication=error", "slave has errored:", JSON.stringify(slaveState.state.lastError));
        throw JSON.stringify(slaveState.state.lastError);
      }

      if (!slaveState.state.running) {
        console.topic("replication=error", "slave is not running");
        break;
      }

      if (compareTicks(slaveState.state.lastAppliedContinuousTick, state.lastLogTick) >= 0 ||
          compareTicks(slaveState.state.lastProcessedContinuousTick, state.lastLogTick) >= 0) { // ||
        console.topic("replication=debug", "slave has caught up. state.lastLogTick:", state.lastLogTick, "slaveState.lastAppliedContinuousTick:", slaveState.state.lastAppliedContinuousTick, "slaveState.lastProcessedContinuousTick:", slaveState.state.lastProcessedContinuousTick);
        break;
      }
        
      if (!printed) {
        console.topic("replication=debug", "waiting for slave to catch up");
        printed = true;
      }
      internal.wait(0.5, false);
    }

    db._flushCache();
    slaveFuncFinal(state);
  };

  return {

    setUp: function() {
      db._useDatabase("_system");
      connectToMaster();
      try {
        db._dropDatabase(cn);
      } catch (err) {}

      db._createDatabase(cn);
      db._useDatabase(cn);

      db._useDatabase("_system");
      connectToSlave();
      
      try {
        db._dropDatabase(cn);
      } catch (err) {}

      db._createDatabase(cn);
    },

    tearDown: function() {
      db._useDatabase("_system");
      connectToMaster();

      db._useDatabase(cn);
      connectToSlave();
      replication.applier.stop();
      replication.applier.forget();
      
      db._useDatabase("_system");
      db._dropDatabase(cn);
    },
    
    testFuzz: function() {
      db._useDatabase(cn);
      connectToMaster();

      compare(
        function(state) {
        },

        function(state) {
          let pickDatabase = function() {
            db._useDatabase('_system');
            let dbs;
            while (true) {
              dbs = db._databases().filter(function(db) { return db !== '_system'; });
              if (dbs.length !== 0) {
                break;
              }
              createDatabase();
            }
            let d = dbs[Math.floor(Math.random() * dbs.length)];
            db._useDatabase(d);
          };
          
          let pickCollection = function() {
            let collections;
            while (true) {
              collections = db._collections().filter(function(c) { return c.name()[0] !== '_' && c.type() === 2; });
              if (collections.length !== 0) {
                break;
              }
              return createCollection();
            }
            return collections[Math.floor(Math.random() * collections.length)];
          };
          
          let pickEdgeCollection = function() {
            let collections;
            while (true) {
              collections = db._collections().filter(function(c) { return c.name()[0] !== '_' && c.type() === 3; });
              if (collections.length !== 0) {
                break;
              }
              return createEdgeCollection();
            }
            return collections[Math.floor(Math.random() * collections.length)];
          };

          let insert = function() {
            let collection = pickCollection();
            emit("insert " + db._name() + " " + collection.name());
            collection.insert({ value: Date.now() });
          };
          
          let insertOverwrite = function() {
            let collection = pickCollection();
            emit("insertOverwrite " + db._name() + " " + collection.name());
            collection.insert({ _key: "test", value: Date.now() }, { overwrite: true });
          };
          
          let remove = function() {
            let collection = pickCollection();
            emit("remove " + db._name() + " " + collection.name());
            if (collection.count() === 0) {
              let k = collection.insert({});
              collection.remove(k);
              return;
            }
            collection.remove(collection.any());
          };

          let replace = function() {
            let collection = pickCollection();
            emit("replace " + db._name() + " " + collection.name());
            if (collection.count() === 0) {
              let k = collection.insert({});
              collection.replace(k, { value2: Date.now() });
              return;
            }
            collection.replace(collection.any(), { value2: Date.now() });
          };
          
          let update = function() {
            let collection = pickCollection();
            emit("update " + db._name() + " " + collection.name());
            if (collection.count() === 0) {
              let k = collection.insert({});
              collection.update(k, { value2: Date.now() });
              return;
            }
            collection.update(collection.any(), { value2: Date.now() });
          };
          
          let insertEdge = function() {
            let collection = pickEdgeCollection();
            emit("insertEdge " + " " + db._name() + " " + collection.name());
            collection.insert({ _from: "test/v1", _to: "test/v2", value: Date.now() });
          };
          
          let insertOrReplace = function() {
            let collection = pickCollection();
            emit("insertOrReplace " + db._name() + " " + collection.name());
            db._executeTransaction({
              collections: { write: [collection.name()] },
              action: function(params) {
                let collection = params.cn, db = require("internal").db;
                let key = "test" + Math.floor(Math.random() * 10000);
                try {
                  db[collection].insert({ _key: key, value: Date.now() });
                } catch (err) {
                  db[collection].replace(key, { value2: Date.now() });
                }
              },
              params: { cn: collection.name() }
            });
          };
          
          let insertOrUpdate = function() {
            let collection = pickCollection();
            emit("insertOrUpdate " + db._name() + " " + collection.name());
            db._executeTransaction({
              collections: { write: [collection.name()] },
              action: function(params) {
                let collection = params.cn, db = require("internal").db;
                let key = "test" + Math.floor(Math.random() * 10000);
                try {
                  db[collection].insert({ _key: key, value: Date.now() });
                } catch (err) {
                  db[collection].update(key, { value2: Date.now() });
                }
              },
              params: { cn: collection.name() }
            });
          };
          
          let insertMulti = function() {
            let collection = pickCollection();
            emit("insertMulti " + db._name() + " " + collection.name());
            db._executeTransaction({
              collections: { write: [collection.name()] },
              action: function(params) {
                let collection = params.cn, db = require("internal").db;
                db[collection].insert({ value1: Date.now() });
                db[collection].insert({ value2: Date.now() });
              },
              params: { cn: collection.name() }
            });
          };
          
          let removeMulti = function() {
            let collection = pickCollection();
            emit("removeMulti " + db._name() + " " + collection.name());
            db._executeTransaction({
              collections: { write: [collection.name()] },
              action: function(params) {
                let collection = params.cn, db = require("internal").db;
                if (db[collection].count() < 2) {
                  let k1 = db[collection].insert({});
                  let k2 = db[collection].insert({});
                  db[collection].remove(k1);
                  db[collection].remove(k2);
                  return;
                }
                db[collection].remove(db[collection].any());
                db[collection].remove(db[collection].any());
              },
              params: { cn: collection.name() }
            });
          };
          
          let removeInsert = function() {
            let collection = pickCollection();
            emit("removeInsert " + db._name() + " " + collection.name());
            db._executeTransaction({
              collections: { write: [collection.name()] },
              action: function(params) {
                let collection = params.cn, db = require("internal").db;
                if (db[collection].count() === 0) {
                  db[collection].insert({ value: Date.now() });
                }
                db[collection].remove(db[collection].any());
                db[collection].insert({ value: Date.now() });
              },
              params: { cn: collection.name() }
            });
          };
          
          let insertRemove = function() {
            let collection = pickCollection();
            emit("insertRemove " + db._name() + " " + collection.name());
            db._executeTransaction({
              collections: { write: [collection.name()] },
              action: function(params) {
                let collection = params.cn, db = require("internal").db;
                let k = db[collection].insert({ value: Date.now() });
                db[collection].remove(k);
              },
              params: { cn: collection.name() }
            });
          };
          
          let insertBatch = function() {
            let collection = pickCollection();
            emit("insertBatch " + db._name() + " " + collection.name());
            db._executeTransaction({
              collections: { write: [collection.name()] },
              action: function(params) {
                let collection = params.cn, db = require("internal").db;
                for (let i = 0; i < 1000; ++i) {
                  db[collection].insert({ value1: Date.now() });
                }
              },
              params: { cn: collection.name() }
            });
          };
          
          
          let createCollection = function() {
            let name = "test" + internal.genRandomAlphaNumbers(16) + Date.now();
            emit("createCollection " + db._name() + " " + name);
            return db._create(name);
          };
          
          let createEdgeCollection = function() {
            let name = "edge" + internal.genRandomAlphaNumbers(16) + Date.now();
            emit("createEdgeCollection " + db._name() + " " + name);
            return db._createEdgeCollection(name);
          };
          
          let dropCollection = function() {
            let collection = pickCollection();
            if (db._name() === cn) {
              // don't drop collections from our test database
              return false;
            }
            emit("dropCollection " + db._name() + " " + collection.name());
            db._drop(collection.name());
          };
          
          let renameCollection = function() {
            let name = internal.genRandomAlphaNumbers(16) + Date.now();
            let collection = pickCollection();
            emit("renameCollection " + db._name() + " " + collection.name() + " " + name);
            collection.rename("fuchs" + name);
          };
          
          let changeCollection = function() {
            let collection = pickCollection();
            emit("changeCollection " + db._name() + " " + collection.name());
            collection.properties({ waitForSync: false });
          };
          
          let truncateCollection = function() {
            let collection = pickCollection();
            emit("truncateCollection " + db._name() + " " + collection.name());
            collection.truncate({ compact: false });
          };

          let createIndex = function () {
            let name = internal.genRandomAlphaNumbers(16) + Date.now();
            let collection = pickCollection();
            emit("createIndex " + db._name() + " " + collection.name());
            collection.ensureIndex({ 
              type: Math.random() >= 0.5 ? "hash" : "skiplist", 
              fields: [ name ],
              sparse: Math.random() > 0.5 
            }); 
          };

          let dropIndex = function () {
            let collection = pickCollection();
            let indexes = collection.getIndexes();
            if (indexes.length > 1) {
              emit("dropIndex " + db._name() + " " + collection.name());
              collection.dropIndex(indexes[1]);
            }
          };

          let createDatabase = function() {
            db._useDatabase('_system');
            let name = "test" + internal.genRandomAlphaNumbers(16) + Date.now();
            emit("createDatabase " + db._name() + " " + name);
            return db._createDatabase(name);
          };

          let dropDatabase = function () {
            pickDatabase();
            let name = db._name();
            if (name === cn) {
              // we do not want to delete our test database
              return;
            }
            db._useDatabase('_system');
            emit("dropDatabase " + db._name() + " " + name);
            db._dropDatabase(name);
          };


          let ops = [
            { name: "insert", func: insert },
            { name: "insertOverwrite", func: insertOverwrite },
            { name: "remove", func: remove },
            { name: "replace", func: replace },
            { name: "update", func: update },
            { name: "insertEdge", func: insertEdge },
            { name: "insertOrReplace", func: insertOrReplace },
            { name: "insertOrUpdate", func: insertOrUpdate },
            { name: "insertMulti", func: insertMulti },
            { name: "removeMulti", func: removeMulti },
            { name: "removeInsert", func: removeInsert },
            { name: "insertRemove", func: insertRemove },
            { name: "insertBatch", func: insertBatch },
            { name: "createCollection", func: createCollection },
            { name: "dropCollection", func: dropCollection },
            { name: "renameCollection", func: renameCollection },
            { name: "changeCollection", func: changeCollection },
            { name: "truncateCollection", func: truncateCollection },
            { name: "createIndex", func: createIndex },
            { name: "dropIndex", func: dropIndex },
            { name: "createDatabase", func: createDatabase },
            { name: "dropDatabase", func: dropDatabase },
          ];
     
          for (let i = 0; i < 3000; ++i) { 
            pickDatabase();
            let op = ops[Math.floor(Math.random() * ops.length)];
            op.func();
          }

          db._useDatabase(cn);
          let total = "";
          emit("leader collections in " + cn);
          db._collections().filter(function(c) { return c.name()[0] !== '_'; }).forEach(function(c) {
            total += c.name() + "-" + c.count() + "-" + collectionChecksum(c.name());
            emit("- " + c.name() + " " + c.count());
            c.indexes().forEach(function(index) {
              emit("  - " + index.type + " " + JSON.stringify(index.fields));
              delete index.selectivityEstimate;
              total += index.type + "-" + JSON.stringify(index.fields);
            });
          });
          state.state = total;
        },

        function(state) {
          db._useDatabase(cn);
          let total = "";
          emit("follower collections in " + cn);
          db._collections().filter(function(c) { return c.name()[0] !== '_'; }).forEach(function(c) {
            total += c.name() + "-" + c.count() + "-" + collectionChecksum(c.name());
            emit("- " + c.name() + " " + c.count());
            c.indexes().forEach(function(index) {
              emit("  - " + index.type + " " + JSON.stringify(index.fields));
              delete index.selectivityEstimate;
              total += index.type + "-" + JSON.stringify(index.fields);
            });
          });
          assertEqual(total, state.state);
        }
      );
    }

  };
}

////////////////////////////////////////////////////////////////////////////////
/// @brief executes the test suite
////////////////////////////////////////////////////////////////////////////////

jsunity.run(ReplicationSuite);

return jsunity.done();
