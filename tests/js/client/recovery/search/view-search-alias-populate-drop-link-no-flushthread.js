/* jshint globalstrict:false, strict:false, unused : false */
/* global runSetup assertEqual, assertTrue, assertFalse, assertNull, fail */
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
/// @author Andrey Abramov
/// @author Copyright 2022, triAGENS GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

var db = require('@arangodb').db;
var internal = require('internal');
var jsunity = require('jsunity');

if (runSetup === true) {
  'use strict';
  global.instanceManager.debugClearFailAt();

  db._drop('UnitTestsRecoveryDummy');
  var c = db._create('UnitTestsRecoveryDummy');
  var i1 = c.ensureIndex({ type: "inverted", name: "i1", includeAllFields:true });

  db._dropView('UnitTestsRecoveryView');
  var v = db._createView('UnitTestsRecoveryView', 'search-alias', {});

  var meta = { indexes: [ { index: i1.name, collection: c.name() } ] };
  v.properties(meta);

  for (let i = 0; i < 10000; i++) {
    c.save({ a: "foo_" + i, b: "bar_" + i, c: i });
  }

  internal.wal.flush(true, true);
  global.instanceManager.debugSetFailAt("RocksDBBackgroundThread::run");
  internal.wait(2); // make sure failure point takes effect

  c.dropIndex(i1);

  c.save({ name: 'crashme' }, { waitForSync: true });

  return 0;
}

function recoverySuite () {
  'use strict';
  jsunity.jsUnity.attachAssertions();

  return {


    testIResearchLinkPopulateDropLinkNoFlushThread: function () {
      let checkView = function(viewName, indexName) {
        let v = db._view(viewName);
        assertEqual(v.name(), viewName);
        assertEqual(v.type(), 'search-alias');
        let indexes = v.properties().indexes;
        assertEqual(0, indexes.length);
      };
      checkView("UnitTestsRecoveryView");

      var result = db._query("FOR doc IN UnitTestsRecoveryView SEARCH doc.c >= 0 COLLECT WITH COUNT INTO length RETURN length").toArray();
      assertEqual(result[0], 0);
    }
  };
}

jsunity.run(recoverySuite);
return jsunity.done();
