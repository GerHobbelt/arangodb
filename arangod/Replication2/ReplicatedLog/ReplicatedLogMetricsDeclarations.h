////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Business Source License 1.1 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     https://github.com/arangodb/arangodb/blob/devel/LICENSE
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

#pragma once

#include "Metrics/CounterBuilder.h"
#include "Metrics/GaugeBuilder.h"
#include "Metrics/HistogramBuilder.h"
#include "Metrics/LogScale.h"

#include <cstdint>

namespace arangodb {

struct AppendEntriesRttScale {
  using scale_t = metrics::LogScale<std::uint64_t>;
  static scale_t scale() {
    // values in us, smallest bucket is up to 1ms, scales up to 2^16ms =~ 65s.
    return {scale_t::kSupplySmallestBucket, 2, 0, 1'000, 16};
  }
};

struct InsertBytesScale {
  using scale_t = metrics::LogScale<std::uint64_t>;
  static scale_t scale() {
    // 1 byte up to 16GiB (1 * 4^17 = 16 * 2^30).
    return {scale_t::kSupplySmallestBucket, 4, 0, 1, 17};
  }
};

struct AppendEntriesNumEntriesScale {
  using scale_t = metrics::LogScale<std::uint64_t>;
  static scale_t scale() {
    return {scale_t::kSupplySmallestBucket, 2, 0, 1, 16};
  }
};

struct AppendEntriesSizeScale {
  using scale_t = metrics::LogScale<std::uint64_t>;
  static scale_t scale() {
    return {scale_t::kSupplySmallestBucket, 2, 0, 64, 18};
  }
};

DECLARE_GAUGE(arangodb_replication2_replicated_log_number, std::uint64_t,
              "Number of replicated logs on this arangodb instance");

DECLARE_HISTOGRAM(arangodb_replication2_replicated_log_append_entries_rtt,
                  AppendEntriesRttScale, "RTT for AppendEntries requests [us]");
DECLARE_HISTOGRAM(
    arangodb_replication2_replicated_log_follower_append_entries_rt,
    AppendEntriesRttScale, "RT for AppendEntries call [us]");

DECLARE_GAUGE(arangodb_replication2_leader_in_memory_entries, std::uint64_t,
              "Number of log entries stored on memory");
DECLARE_GAUGE(arangodb_replication2_leader_in_memory_bytes, std::size_t,
              "Size of log entries stored on memory");

DECLARE_COUNTER(arangodb_replication2_replicated_log_creation_total,
                "Number of replicated logs created since server start");

DECLARE_COUNTER(arangodb_replication2_replicated_log_deletion_total,
                "Number of replicated logs deleted since server start");

DECLARE_GAUGE(
    arangodb_replication2_replicated_log_leader_number, std::uint64_t,
    "Number of replicated logs this server has, and is currently a leader of");

DECLARE_GAUGE(arangodb_replication2_replicated_log_follower_number,
              std::uint64_t,
              "Number of replicated logs this server has, and is currently a "
              "follower of");

DECLARE_GAUGE(arangodb_replication2_replicated_log_inactive_number,
              std::uint64_t,
              "Number of replicated logs this server has, and is currently "
              "neither leader nor follower of");

DECLARE_COUNTER(arangodb_replication2_replicated_log_leader_took_over_total,
                "Number of times a replicated log on this server took over as "
                "leader in a term");

DECLARE_COUNTER(arangodb_replication2_replicated_log_started_following_total,
                "Number of times a replicated log on this server started "
                "following a leader in a term");

DECLARE_HISTOGRAM(arangodb_replication2_replicated_log_inserts_bytes,
                  InsertBytesScale,
                  "Number of bytes per insert in replicated log leader "
                  "instances on this server [bytes]");

DECLARE_HISTOGRAM(
    arangodb_replication2_replicated_log_inserts_rtt, AppendEntriesRttScale,
    "Histogram of round-trip times of replicated log inserts [us]");

DECLARE_HISTOGRAM(
    arangodb_replication2_replicated_log_append_entries_num_entries,
    AppendEntriesNumEntriesScale,
    "Histogram of number of log entries per append-entries request");
DECLARE_HISTOGRAM(arangodb_replication2_replicated_log_append_entries_size,
                  AppendEntriesSizeScale,
                  "Histogram of size of append-entries requests");
DECLARE_COUNTER(
    arangodb_replication2_replicated_log_follower_entry_drop_total,
    "Number of log entries dropped by a follower before appending the log");

DECLARE_COUNTER(
    arangodb_replication2_replicated_log_leader_append_entries_error_total,
    "Number of failed append-entries requests");

DECLARE_COUNTER(
    arangodb_replication2_replicated_log_number_accepted_entries_total,
    "Number of accepted (not yet committed) log entries");
DECLARE_COUNTER(
    arangodb_replication2_replicated_log_number_committed_entries_total,
    "Number of committed log entries");
DECLARE_COUNTER(arangodb_replication2_replicated_log_number_meta_entries_total,
                "Number of meta log entries");
DECLARE_COUNTER(
    arangodb_replication2_replicated_log_number_compacted_entries_total,
    "Number of compacted log entries");

}  // namespace arangodb
