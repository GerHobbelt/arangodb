/* jshint globalstrict:false, strict:false, maxlen: 5000 */
/* global describe, beforeEach, afterEach, it, global, before,  */
'use strict';

// //////////////////////////////////////////////////////////////////////////////
// / DISCLAIMER
// /
// / Copyright 2020 ArangoDB GmbH, Cologne, Germany
// /
// / Licensed under the Apache License, Version 2.0 (the "License")
// / you may not use this file except in compliance with the License.
// / You may obtain a copy of the License at
// /
// /     http://www.apache.org/licenses/LICENSE-2.0
// /
// / Unless required by applicable law or agreed to in writing, software
// / distributed under the License is distributed on an "AS IS" BASIS,
// / WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// / See the License for the specific language governing permissions and
// / limitations under the License.
// /
// / Copyright holder is ArangoDB GmbH, Cologne, Germany
// /
// / @author Michael Hackstein
// //////////////////////////////////////////////////////////////////////////////

const {expect} = require('chai');
const request = require("@arangodb/request");
const db = require("internal").db;

class Watcher {
  constructor(metric) {
    this._metric = metric;
    this._before = null;
    this._after = null;
  }

  before(metrics) {
    this._before = metrics[this._metric];
  };

  after(metrics) {
    this._after =  metrics[this._metric];
  };

  check(){
    expect(this._after).to.be.greaterThan(this._before);
  };
  
  checkEq(increment){
    expect(this._after).to.be.equal(this._before + increment);
  };

  checkAtLeast(minIncrement){
    expect(this._after).to.be.at.least(this._before + minIncrement);
  };
}

const expectOneBucketChanged = (actual, old) => {
  let foundOne = false;
  for (const [key, value] of Object.entries(actual)) {
    if (old[key] < value) {
      foundOne = true;
    }
    expect(old[key]).to.be.most(value);

  }
  expect(foundOne).to.equal(true);
};

class BucketWatcher extends Watcher{  

  check(){
    expectOneBucketChanged(this._after, this._before);
  };
}

const MetricNames = {
  QUERY_TIME: "arangodb_aql_total_query_time_msec",
  PHASE_1_BUCKET: "arangodb_maintenance_phase1_runtime_msec_bucket",
  PHASE_1_COUNT: "arangodb_maintenance_phase1_runtime_msec_count",
  PHASE_2_BUCKET: "arangodb_maintenance_phase2_runtime_msec_bucket",
  PHASE_2_COUNT: "arangodb_maintenance_phase2_runtime_msec_count",
  SHARD_COUNT: "arangodb_shards_total_count",
  SHARD_LEADER_COUNT: "arangodb_shards_leader_count",
  HEARTBEAT_BUCKET: "arangodb_heartbeat_send_time_msec_bucket",
  HEARTBEAT_COUNT: "arangodb_heartbeat_send_time_msec_count",
  SUPERVISION_BUCKET: "arangodb_agency_supervision_runtime_msec_bucket",
  SUPERVISION_COUNT: "arangodb_agency_supervision_runtime_msec_count",
  SLOW_QUERY_COUNT: "arangodb_aql_slow_query",

  HTTP_DELETE_COUNT: "arangodb_http_request_statistics_http_delete_requests",
  HTTP_GET_COUNT: "arangodb_http_request_statistics_http_get_requests",
  HTTP_HEAD_COUNT: "arangodb_http_request_statistics_http_head_requests",
  HTTP_OPTIONS_COUNT: "arangodb_http_request_statistics_http_options_requests",
  HTTP_PATCH_COUNT: "arangodb_http_request_statistics_http_patch_requests",
  HTTP_POST_COUNT: "arangodb_http_request_statistics_http_post_requests",
  HTTP_PUT_COUNT: "arangodb_http_request_statistics_http_put_requests",
  HTTP_OTHER_COUNT: "arangodb_http_request_statistics_other_http_requests",
  HTTP_TOTAL_COUNT: "arangodb_http_request_statistics_total_requests",

  CLIENT_CONNECTIONS_COUNT: "arangodb_client_connection_statistics_client_connections",

  IO_TIME_BUCKET: "arangodb_client_connection_statistics_io_time_bucket",
  REQUST_TIME_BUCKET: "arangodb_client_connection_statistics_request_time_bucket",
  
  BYTES_RECIVED_BUCKET: "arangodb_client_connection_statistics_bytes_received_bucket",
  BYTES_SENT_BUCKET: "arangodb_client_connection_statistics_bytes_sent_bucket",
  TOTAL_TIME_BUCKET: "arangodb_client_connection_statistics_total_time_bucket",


  //METRIC: "arangodb_client_connection_statistics_bytes_received_sum",
  // BMETRIC: "arangodb_client_connection_statistics_bytes_received_bucket",

  AGENCY_LOG_SIZE: "arangodb_agency_log_size_bytes"
};

class ConnectionStatWatcher {
  constructor(){
    // this.CountWatcher = new Watcher (MetricNames.METRIC);
    // this.BucketWatcher = new BucketWatcher (MetricNames.BMETRIC);

    this.IoTimeBucketWatcher = new BucketWatcher (MetricNames.IO_TIME_BUCKET);
    this.RequestTimeBucketWatcher = new BucketWatcher (MetricNames.REQUST_TIME_BUCKET);
    
    this.BytesRecivedBucketWatcher = new BucketWatcher (MetricNames.BYTES_RECIVED_BUCKET); 
    this.BytesSentBucketWatcher = new BucketWatcher (MetricNames.BYTES_SENT_BUCKET); 
    this.TotalTimeBucketWatcher = new BucketWatcher (MetricNames.TOTAL_TIME_BUCKET); 
  }

  before(metrics){
    //this.CountWatcher.before(metrics);
    // this.BucketWatcher.before(metrics);
    this.IoTimeBucketWatcher.before(metrics);
    this.RequestTimeBucketWatcher.before(metrics);

    this.BytesRecivedBucketWatcher.before(metrics);
    this.BytesSentBucketWatcher.before(metrics);
    this.TotalTimeBucketWatcher.before(metrics);
        
  }

  after(metrics){
    //this.CountWatcher.after(metrics);
    // this.BucketWatcher.after(metrics);
    this.IoTimeBucketWatcher.after(metrics);
    this.RequestTimeBucketWatcher.after(metrics);

    this.BytesRecivedBucketWatcher.after(metrics);
    this.BytesSentBucketWatcher.after(metrics);
    this.TotalTimeBucketWatcher.after(metrics);
    
  }

  check(){
    this.CountWatcher.check();
    //this.BucketWatcher.check();
    
    this.IoTimeBucketWatcher.check();
    this.RequestTimeBucketWatcher.check();

    this.BytesRecivedBucketWatcher.check();
    this.BytesSentBucketWatcher.check();
    this.TotalTimeBucketWatcher.check();
  }
}


class HttpRequestsCountWatcher {
  constructor(){
    this.HttpDeleteCountWatcher = new Watcher (MetricNames.HTTP_DELETE_COUNT);
    this.HttpGetCountWatcher = new Watcher (MetricNames.HTTP_GET_COUNT);
    this.HttpHeadCountWatcher = new Watcher (MetricNames.HTTP_HEAD_COUNT);
    this.HttpOptionsCountWatcher = new Watcher (MetricNames.HTTP_OPTIONS_COUNT);
    this.HttpPatchCountWatcher = new Watcher (MetricNames.HTTP_PATCH_COUNT);
    this.HttpPostCountWatcher = new Watcher (MetricNames.HTTP_POST_COUNT);
    this.HttpPutCountWatcher = new Watcher (MetricNames.HTTP_PUT_COUNT);
    this.HttpOtherCountWatcher = new Watcher (MetricNames.HTTP_OTHER_COUNT);
    this.HttpTotalCountWatcher = new Watcher (MetricNames.HTTP_TOTAL_COUNT);       
  }

  before(metrics){
    this.HttpDeleteCountWatcher.before(metrics);
    this.HttpGetCountWatcher.before(metrics);
    this.HttpHeadCountWatcher.before(metrics);
    this.HttpOptionsCountWatcher.before(metrics);
    this.HttpPatchCountWatcher.before(metrics);
    this.HttpPostCountWatcher.before(metrics);
    this.HttpPutCountWatcher.before(metrics);
    this.HttpOtherCountWatcher.before(metrics);
    this.HttpTotalCountWatcher.before(metrics);
  }
  
  after(metrics){
    this.HttpDeleteCountWatcher.after(metrics);
    this.HttpGetCountWatcher.after(metrics);
    this.HttpHeadCountWatcher.after(metrics);
    this.HttpOptionsCountWatcher.after(metrics);
    this.HttpPatchCountWatcher.after(metrics);
    this.HttpPostCountWatcher.after(metrics);
    this.HttpPutCountWatcher.after(metrics);
    this.HttpOtherCountWatcher.after(metrics);
    this.HttpTotalCountWatcher.after(metrics);
  }

  check(){
    this.HttpDeleteCountWatcher.checkEq(1);
    this.HttpGetCountWatcher.checkEq(1);
    this.HttpHeadCountWatcher.checkEq(1);
    this.HttpOptionsCountWatcher.checkEq(1);
    this.HttpPatchCountWatcher.checkEq(1);
    this.HttpPostCountWatcher.checkEq(2);
    this.HttpPutCountWatcher.checkAtLeast(1);
    this.HttpOtherCountWatcher.checkEq(1);
    this.HttpTotalCountWatcher.checkAtLeast(9);
  }
}


class AgencyLogSizeWatcher extends Watcher {
  constructor() {
    super(MetricNames.AGENCY_LOG_SIZE);
  }
}

class QueryTimeWatcher extends Watcher {
  constructor(minChange) {
    super(MetricNames.QUERY_TIME);
    this._minChange = minChange;
  };

  check (){    
    expect(this._after).to.be.at.least(this._before + this._minChange);
  };
}

class SlowQueryCountWatcher extends Watcher {
  constructor(minChange) {
    super(MetricNames.SLOW_QUERY_COUNT);
    this._minChange = minChange;
  };
  
  check (){
    expect(this._after).to.be.equal(this._before + this._minChange);    
  };
}


class ShardCountWatcher extends Watcher {
  constructor(change) {
    super(MetricNames.SHARD_COUNT);
    this._change = change;
  }

  check (){
    expect(this._after).to.be.equal(this._before + this._change);
  };
}


class ShardLeaderCountWatcher extends Watcher {
  constructor(change) {
    super(MetricNames.SHARD_LEADER_COUNT);
    this._change = change;
  }

  check (){
    expect(this._after).to.be.equal(this._before + this._change);
  }
}

class MaintenanceWatcher {
  constructor() {
    this._p1ValueWatcher = new Watcher(MetricNames.PHASE_1_COUNT);
    this._p2ValueWatcher = new Watcher(MetricNames.PHASE_2_COUNT);
    this._p1BucketWatcher = new BucketWatcher(MetricNames.PHASE_1_BUCKET);
    this._p2BucketWatcher = new BucketWatcher(MetricNames.PHASE_2_BUCKET);
  }

  before(metrics) {
    this._p1ValueWatcher.before(metrics);
    this._p2ValueWatcher.before(metrics);
    this._p1BucketWatcher.before(metrics);
    this._p2BucketWatcher.before(metrics);
  };

  after(metrics) {
    this._p1ValueWatcher.after(metrics);
    this._p2ValueWatcher.after(metrics);
    this._p1BucketWatcher.after(metrics);
    this._p2BucketWatcher.after(metrics);
  };

  check(){
    this._p1ValueWatcher.check();
    this._p2ValueWatcher.check();
    this._p1BucketWatcher.check();
    this._p2BucketWatcher.check();
  }
}

class HeartBeatWatcher {
  constructor() {
    this._ValueWatcher = new Watcher(MetricNames.HEARTBEAT_COUNT);
    this._BucketWatcher = new BucketWatcher(MetricNames.HEARTBEAT_BUCKET);
  }

  before(metrics) {
    this._ValueWatcher.before(metrics);
    this._BucketWatcher.before(metrics);
  };

  after(metrics) {
    this._ValueWatcher.after(metrics);
    this._BucketWatcher.after(metrics);
  };

  check(){
    this._ValueWatcher.check();
    this._BucketWatcher.check();
  }
}

class SupervisionWatcher {
  constructor() {
    this._svValueWatcher = new Watcher(MetricNames.SUPERVISION_COUNT);
    this._svBucketWatcher = new BucketWatcher(MetricNames.SUPERVISION_BUCKET);
  }

  before(metrics) {
    this._svValueWatcher.before(metrics);
    this._svBucketWatcher.before(metrics);
  };

  after(metrics) {
    this._svValueWatcher.after(metrics);
    this._svBucketWatcher.after(metrics);
  };

  check(){
    this._svValueWatcher.check();
    this._svBucketWatcher.check();
  };
};


describe('_admin/metrics', () => {

  const getServers = () => {
    const endpointToURL = (endpoint) => {
      if (endpoint.substr(0, 6) === 'ssl://') {
        return 'https://' + endpoint.substr(6);
      }
      var pos = endpoint.indexOf('://');
      if (pos === -1) {
        return 'http://' + endpoint;
      }
      return 'http' + endpoint.substr(pos);
    };
    const {instanceInfo} = global;
    const list = new Map();
    list.set("coordinator", []);
    list.set("dbserver", []);
    list.set("agent", []);
    for (const d of instanceInfo.arangods) {
      const {role, endpoint} = d;
      list.get(role).push(endpointToURL(endpoint));
    }
    return list;
  };

  let servers;

  before(() => {
    servers = getServers();
  });

  const extractKeyAndLabel = (key) => {
    const start = key.indexOf('{');
    const labels = new Map();
    if (start === -1) {
      return [key, labels];
    }
    const labelPart = key.substring(start + 1, key.length - 1);
    for (const l of labelPart.split(",")) {
      const [lab, val] = l.split("=");
      labels.set(lab,val);
    }
    return [
      key.substring(0, start),
      labels
    ];
  };

  const prometheusToJson = (prometheus) => {
    const lines = prometheus.split('\n').filter((s) => !s.startsWith('#') && s !== '');
    const res = {};
    for (const l of lines) {
      const [keypart, count] = l.split(' ');
      const [key, labels] = extractKeyAndLabel(keypart);
      if (labels.has("le")) {
        // Bucket case
        // We only check for:
        // identifier{le="range"}
        // For all other bucket-types this code will fail
        res[key] = res[key] || {};
        res[key][labels.get("le")] = parseFloat(count);
      } else {
        // evertyhing else
        // We ignore other labels for now.
        res[key] = parseFloat(count);
      }

    }
    return res;
  };

  const loadMetrics = (role, idx) =>  {
    const url = `${servers.get(role)[idx]}/_admin/metrics`;

    const res = request({
      json: true,
      method: 'GET',
      url
    });
    expect(res.statusCode).to.equal(200);
    return prometheusToJson(res.body);
  };

  const joinMetrics = (lhs, rhs) => {
    if (Object.entries(lhs).length === 0) {
      return rhs;
    }
    for (const [key, value] of Object.entries(rhs)) {
      if (value instanceof Object) {
        for (const [bucket, content] of Object.entries(value)) {
          lhs[key][bucket] += content;
        }
      } else {
        lhs[key] += rhs[key];
      }
    }
    return lhs;
  };

  const loadAllMetrics = (role) => {
    return servers.get(role).map((_, i) => loadMetrics(role, i)).reduce(joinMetrics, {});
    };

  const runTest = (action, watchers, role) => {
    const metricsBefore = loadAllMetrics(role);
    watchers.forEach(w => {w.before(metricsBefore);});

    action();

    const metricsAfter = loadAllMetrics(role);
    watchers.forEach(w => {
      w.after(metricsAfter);
      w.check();
    });      
  };


  it('http requests statistics',() => {
    
    runTest(() => {
      const request = require("@arangodb/request");      
      const url = `${servers.get('coordinator')[0]}`;
      
      let resCreateDB = request({
        url: `${url}/_api/collection`, 
        method: "POST",
        body: '{"name": "UnitTestCollection"}'        
      });  
          
      expect(resCreateDB.statusCode).to.equal(200);
      require("internal").wait(5.0);

      let resCreateDoc = request({
        url: `${url}/_api/document/UnitTestCollection?waitForSync=true`, 
        method: "POST",
        body: '{"_key": "testDoc", "test": "test"}'
      });  
          
      expect(resCreateDoc.statusCode).to.equal(201);

      let resProp = request({
        url: `${url}/_api/collection/UnitTestCollection/properties`, 
        method: "PUT"
      });

      expect(resProp.statusCode).to.equal(200);

      let resPatchDoc = request({
        url: `${url}/_api/document/UnitTestCollection?waitForSync=true`, 
        method: "PATCH",
        body: '[{"_key": "testDoc", "test": "test2"}]'
      });  
          
      expect(resPatchDoc.statusCode).to.equal(201);

      let resHeadDoc = request({
        url: `${url}/_api/document/UnitTestCollection/testDoc`, 
        method: "HEAD"
      }); 
      expect(resHeadDoc.statusCode).to.equal(200);

      let resOtionsDoc = request({
        url: `${url}/_api`, 
        method: "OPTIONS"
      }); 
      expect(resOtionsDoc.statusCode).to.equal(200);

      let resTraceDoc = request({
        url: `${url}/_api`, 
        method: "TRACE"
      }); 
      expect(resTraceDoc.statusCode).to.equal(500);


      let resDelete = request({
        url: `${url}/_api/collection/UnitTestCollection`, 
        method: "DELETE"        
      });

      expect(resDelete.statusCode).to.equal(200);
      require("internal").wait(5.0);

    }, [new ConnectionStatWatcher()], 'dbserver');
  
  });


  // it('agency log size ', () => {
  //   try{  
  //     runTest(() => {
  //       db._create("UnitTestCollection", {numberOfShards: 9, replicationFactor: 2}, undefined, {waitForSyncReplication: true});
  //         require("internal").wait(5.0);
  //     }, [new AgencyLogSizeWatcher()], 'agent');
  //   } finally {
  //     db._drop("UnitTestCollection");
  //   }  
  // });

  // it('aql query count and slow query count', () => {
  //   runTest(() => {
  //     const queries = require("@arangodb/aql/queries");
  //     const oldThreshold = queries.properties().slowQueryThreshold;
  //     queries.properties({slowQueryThreshold: 1});
  //     db._query(`return sleep(1)`);
  //     queries.properties({slowQueryThreshold: oldThreshold});       
  //   }, [new SlowQueryCountWatcher(1), new QueryTimeWatcher(1000)], 'coordinator');
  // });

  // it('collection and index', () => {
  //   try {
  //     runTest(() => {
  //       db._create("UnitTestCollection", {numberOfShards: 9, replicationFactor: 2}, undefined, {waitForSyncReplication: true});
  //       require("internal").wait(10.0); // database servers update their shard count in phaseOne. So lets wait until all have done their next phaseOne.
  //     },
  //     [new MaintenanceWatcher(), new ShardCountWatcher(18), new ShardLeaderCountWatcher(9)],
  //     "dbserver"
  //     );
  //     runTest(() => {
  //       db["UnitTestCollection"].ensureHashIndex("temp");
  //     },
  //     [new MaintenanceWatcher()],
  //     "dbserver"
  //     );
  //   } finally {
  //     db._drop("UnitTestCollection");
  //   }
  // });

  // it('at least 1 heartbeat and supervision per second', () => {
  //   runTest(() => {
  //     require("internal").wait(1.0);
  //   }, [new HeartBeatWatcher()], "dbserver");

  //   runTest(() => {
  //     require("internal").wait(1.0);
  //   }, [new HeartBeatWatcher()], "coordinator");

  //   runTest(() => {
  //     require("internal").wait(1.0);
  //   }, [new SupervisionWatcher()], "agent");

  // });

    
});
