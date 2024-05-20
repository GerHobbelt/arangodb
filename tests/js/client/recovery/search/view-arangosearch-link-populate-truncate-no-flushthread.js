/* jshint globalstrict:false, strict:false, unused : false */
/* global assertEqual, assertTrue, assertFalse, assertNull, fail */
////////////////////////////////////////////////////////////////////////////////
/// @brief tests for truncate operation over aragosearch link
///
/// @file
///
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
/// @author Andrei Lobov
/// @author Copyright 2020, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

const db = require('@arangodb').db;
const internal = require('internal');
const jsunity = require('jsunity');
const cn = 'UnitTestsTruncateDummy';
const vn = cn + 'View';

function runSetup () {
  'use strict';
  internal.debugClearFailAt();

  db._drop(cn);
  var c = db._create(cn);

  db._dropView(vn);
  var meta = { links: { [cn]: { includeAllFields: true } }, storedValues: ["_key"] };
  db._createView(vn, 'arangosearch', meta);

  internal.wal.flush(true, true);
  internal.debugSetFailAt("RocksDBBackgroundThread::run");
  internal.wait(2); // make sure failure point takes effect

  // 35k to overcome RocksDB optimization and force use truncate
  for (let i = 0; i < 35000; i++) {
    c.save({ a: "foo_" + i, b: "bar_" + i, c: i, _key: "doc_" + i });
  }

  c.save({ name: "crashme" }, { waitForSync: true });
  c.truncate();
  // force sync
  db._query("FOR doc IN " + vn + " SEARCH doc.c >= 0 OPTIONS {waitForSync: true} COLLECT WITH COUNT INTO length RETURN length");
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief test suite
// //////////////////////////////////////////////////////////////////////////////

function recoverySuite () {
  'use strict';
  jsunity.jsUnity.attachAssertions();

  return {


    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test whether we can restore the trx data
    // //////////////////////////////////////////////////////////////////////////////

    testIResearchLinkPopulateTruncate: function () {
      if (db._properties().replicationVersion === "2") {
        // TODO: Temporarily disabled.
        // Should be re-enabled as soon as https://arangodb.atlassian.net/browse/CINFRA-876
        // is fixed.
        return;
      }

      var v = db._view(vn);
      assertEqual(v.name(), vn);
      assertEqual(v.type(), 'arangosearch');
      var p = v.properties().links;
      assertTrue(p.hasOwnProperty(cn));
      var result = db._query("FOR doc IN " + vn + " SEARCH doc.c >= 0 OPTIONS {waitForSync: true} COLLECT WITH COUNT INTO length RETURN length").toArray();
      var expectedResultStoredValues = db._query("FOR doc IN " + vn + " SEARCH doc.c >= 0 OPTIONS {waitForSync: true} RETURN doc._key").toArray();
      var expectedResult = db._query("FOR doc IN " + cn + " FILTER doc.c >= 0 COLLECT WITH COUNT INTO length RETURN length").toArray();
      var expectedResultNonStoredValues = db._query("FOR doc IN " + vn + " SEARCH doc.c >= 0 OPTIONS {waitForSync: true} RETURN doc._id").toArray();
      const message = `Search with COUNT ${result[0]}, Collection COUNT ${expectedResult[0]}, SearchStoredValues: ${expectedResultStoredValues.length}. Search Not StoradeValues: ${expectedResultNonStoredValues.length} StoredValue _key values: ${JSON.stringify(expectedResultStoredValues.sort())}, not stored: ${JSON.stringify(expectedResultNonStoredValues.sort())}`;
      assertEqual(result[0], expectedResult[0], message);
      assertEqual(result[0], expectedResultStoredValues.length, message);
      assertEqual(result[0], expectedResultNonStoredValues.length, message);
    }
 };
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief executes the test suite
// //////////////////////////////////////////////////////////////////////////////

function main (argv) {
  'use strict';
  if (argv[1] === 'setup') {
    runSetup();
    return 0;
  } else {
    jsunity.run(recoverySuite);
    return jsunity.writeDone().status ? 0 : 1;
  }
}
