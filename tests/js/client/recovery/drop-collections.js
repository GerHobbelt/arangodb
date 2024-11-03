/* jshint globalstrict:false, strict:false, unused : false */
/* global runSetup assertNull, assertEqual */

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
// / @author Copyright 2012, triAGENS GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

var db = require('@arangodb').db;
var internal = require('internal');
var jsunity = require('jsunity');

if (runSetup === true) {
  'use strict';
  global.instanceManager.debugClearFailAt();

  var i;
  for (i = 0; i < 5; ++i) {
    db._drop('UnitTestsRecovery' + i);
    db._create('UnitTestsRecovery' + i);
  }

  for (i = 0; i < 4; ++i) {
    db._drop('UnitTestsRecovery' + i);
  }

  db.UnitTestsRecovery4.save({ _key: 'crashme' }, true); // wait for sync

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

    testDropCollections: function () {
      var i;

      for (i = 0; i < 4; ++i) {
        assertNull(db._collection('UnitTestsRecovery' + i));
      }

      assertEqual(1, db._collection('UnitTestsRecovery4').count());
    }

  };
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief executes the test suite
// //////////////////////////////////////////////////////////////////////////////

jsunity.run(recoverySuite);
return jsunity.done();
