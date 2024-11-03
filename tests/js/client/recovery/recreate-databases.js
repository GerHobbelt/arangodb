/* jshint globalstrict:false, strict:false, unused : false */
/* global runSetup assertEqual */
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
// / @author Jan Steemann
// / @author Copyright 2013, triAGENS GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

var db = require('@arangodb').db;
var internal = require('internal');
var jsunity = require('jsunity');

if (runSetup === true) {
  'use strict';
  global.instanceManager.debugClearFailAt();

  db._drop('test');
  db._create('test');

  var i;
  for (i = 0; i < 5; ++i) {
    db._useDatabase('_system');

    try {
      db._dropDatabase('UnitTestsRecovery' + i);
    } catch (err) {
      // ignore this error
    }

    db._createDatabase('UnitTestsRecovery' + i);
    db._useDatabase('UnitTestsRecovery' + i);
    db._create('test');
    db.test.save({ value: i });
  }

  db._useDatabase('_system');

  for (i = 0; i < 5; ++i) {
    db._dropDatabase('UnitTestsRecovery' + i);
  }

  for (i = 0; i < 5; ++i) {
    db._useDatabase('_system');

    try {
      db._dropDatabase('UnitTestsRecovery' + i);
    } catch (err) {
      // ignore this error
    }

    db._createDatabase('UnitTestsRecovery' + i);
    db._useDatabase('UnitTestsRecovery' + i);
    db._create('foo');
    db.foo.save({ value: 'test' + i });
  }

  db._useDatabase('_system');

  db.test.save({ _key: 'crashme' }, true);

  return 0;
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief test suite
// //////////////////////////////////////////////////////////////////////////////

function recoverySuite () {
  'use strict';
  jsunity.jsUnity.attachAssertions();

  return {


    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test whether we the data are correct after restart
    // //////////////////////////////////////////////////////////////////////////////

    testRecreateDatabases: function () {
      var i;
      for (i = 0; i < 5; ++i) {
        db._useDatabase('UnitTestsRecovery' + i);
        var docs = db.foo.toArray();
        assertEqual(1, docs.length);
        assertEqual('test' + i, docs[0].value);
      }
    }

  };
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief executes the test suite
// //////////////////////////////////////////////////////////////////////////////

jsunity.run(recoverySuite);
return jsunity.done();
