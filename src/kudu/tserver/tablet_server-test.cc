// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/tserver/tablet_server-test-base.h"

#include <stdlib.h>
#include <unistd.h>

#include <cstdint>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/bind.hpp>
#include <boost/optional/optional.hpp>
#include <gflags/gflags.h>
#include <gflags/gflags_declare.h>
#include <glog/logging.h>
#include <google/protobuf/util/message_differencer.h>
#include <gtest/gtest.h>

#include "kudu/clock/clock.h"
#include "kudu/clock/hybrid_clock.h"
#include "kudu/common/common.pb.h"
#include "kudu/common/encoded_key.h"
#include "kudu/common/partial_row.h"
#include "kudu/common/partition.h"
#include "kudu/common/row_operations.h"
#include "kudu/common/schema.h"
#include "kudu/common/timestamp.h"
#include "kudu/common/wire_protocol-test-util.h"
#include "kudu/common/wire_protocol.h"
#include "kudu/common/wire_protocol.pb.h"
#include "kudu/consensus/log-test-base.h"
#include "kudu/consensus/log.h"
#include "kudu/consensus/metadata.pb.h"
#include "kudu/consensus/raft_consensus.h"
#include "kudu/fs/block_id.h"
#include "kudu/fs/fs-test-util.h"
#include "kudu/fs/fs.pb.h"
#include "kudu/fs/fs_manager.h"
#include "kudu/gutil/callback.h"
#include "kudu/gutil/casts.h"
#include "kudu/gutil/gscoped_ptr.h"
#include "kudu/gutil/port.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/gutil/stringprintf.h"
#include "kudu/gutil/strings/escaping.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/rpc/messenger.h"
#include "kudu/rpc/rpc_controller.h"
#include "kudu/rpc/rpc_header.pb.h"
#include "kudu/server/rpc_server.h"
#include "kudu/server/server_base.pb.h"
#include "kudu/server/server_base.proxy.h"
#include "kudu/tablet/metadata.pb.h"
#include "kudu/tablet/tablet.h"
#include "kudu/tablet/tablet_metadata.h"
#include "kudu/tablet/tablet_replica.h"
#include "kudu/tserver/heartbeater.h"
#include "kudu/tserver/mini_tablet_server.h"
#include "kudu/tserver/scanners.h"
#include "kudu/tserver/tablet_server.h"
#include "kudu/tserver/tablet_server_options.h"
#include "kudu/tserver/tablet_server_test_util.h"
#include "kudu/tserver/ts_tablet_manager.h"
#include "kudu/tserver/tserver.pb.h"
#include "kudu/tserver/tserver_admin.pb.h"
#include "kudu/tserver/tserver_admin.proxy.h"
#include "kudu/tserver/tserver_service.pb.h"
#include "kudu/tserver/tserver_service.proxy.h"
#include "kudu/util/countdown_latch.h"
#include "kudu/util/crc.h"
#include "kudu/util/curl_util.h"
#include "kudu/util/env.h"
#include "kudu/util/faststring.h"
#include "kudu/util/jsonwriter.h"
#include "kudu/util/metrics.h"
#include "kudu/util/monotime.h"
#include "kudu/util/net/sockaddr.h"
#include "kudu/util/path_util.h"
#include "kudu/util/pb_util.h"
#include "kudu/util/scoped_cleanup.h"
#include "kudu/util/slice.h"
#include "kudu/util/status.h"
#include "kudu/util/stopwatch.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/test_util.h"
#include "kudu/util/thread.h"
#include "kudu/util/zlib.h"

using google::protobuf::util::MessageDifferencer;
using kudu::clock::Clock;
using kudu::clock::HybridClock;
using kudu::consensus::ConsensusStatePB;
using kudu::fs::CreateCorruptBlock;
using kudu::pb_util::SecureDebugString;
using kudu::pb_util::SecureShortDebugString;
using kudu::rpc::Messenger;
using kudu::rpc::MessengerBuilder;
using kudu::rpc::RpcController;
using kudu::tablet::RowSetDataPB;
using kudu::tablet::Tablet;
using kudu::tablet::TabletReplica;
using kudu::tablet::TabletSuperBlockPB;
using std::set;
using std::shared_ptr;
using std::string;
using std::thread;
using std::unique_ptr;
using std::vector;
using strings::Substitute;

DEFINE_int32(single_threaded_insert_latency_bench_warmup_rows, 100,
             "Number of rows to insert in the warmup phase of the single threaded"
             " tablet server insert latency micro-benchmark");

DEFINE_int32(single_threaded_insert_latency_bench_insert_rows, 1000,
             "Number of rows to insert in the testing phase of the single threaded"
             " tablet server insert latency micro-benchmark");

DEFINE_int32(delete_tablet_bench_num_flushes, 200,
             "Number of disk row sets to flush in the delete tablet benchmark");

DECLARE_bool(crash_on_eio);
DECLARE_bool(enable_maintenance_manager);
DECLARE_bool(fail_dns_resolution);
DECLARE_double(env_inject_eio);
DECLARE_int32(flush_threshold_mb);
DECLARE_int32(flush_threshold_secs);
DECLARE_int32(maintenance_manager_num_threads);
DECLARE_int32(metrics_retirement_age_ms);
DECLARE_int32(scanner_batch_size_rows);
DECLARE_int32(scanner_gc_check_interval_us);
DECLARE_int32(scanner_ttl_ms);
DECLARE_string(block_manager);
DECLARE_string(env_inject_eio_globs);

// Declare these metrics prototypes for simpler unit testing of their behavior.
METRIC_DECLARE_counter(rows_inserted);
METRIC_DECLARE_counter(rows_updated);
METRIC_DECLARE_counter(rows_deleted);
METRIC_DECLARE_counter(scanners_expired);
METRIC_DECLARE_gauge_uint64(log_block_manager_blocks_under_management);
METRIC_DECLARE_gauge_uint64(log_block_manager_containers);
METRIC_DECLARE_counter(log_block_manager_holes_punched);
METRIC_DECLARE_gauge_size(active_scanners);
METRIC_DECLARE_gauge_size(tablet_active_scanners);

namespace kudu {

namespace tablet {
class RowSet;
}

namespace tserver {

class TabletServerTest : public TabletServerTestBase {
 public:
  // Starts the tablet server, override to start it later.
  virtual void SetUp() OVERRIDE {
    NO_FATALS(TabletServerTestBase::SetUp());
    NO_FATALS(StartTabletServer(/* num_data_dirs */ 1));
  }

  void DoOrderedScanTest(const Schema& projection, const string& expected_rows_as_string);

  void ScanYourWritesTest(uint64_t propagated_timestamp, ScanResponsePB* resp);
};

TEST_F(TabletServerTest, TestPingServer) {
  // Ping the server.
  PingRequestPB req;
  PingResponsePB resp;
  RpcController controller;
  ASSERT_OK(proxy_->Ping(req, &resp, &controller));
}

TEST_F(TabletServerTest, TestStatus) {
  // Get the server's status.
  server::GetStatusRequestPB req;
  server::GetStatusResponsePB resp;
  RpcController controller;
  ASSERT_OK(generic_proxy_->GetStatus(req, &resp, &controller));
  ASSERT_TRUE(resp.has_status());
  ASSERT_TRUE(resp.status().has_node_instance());
  ASSERT_EQ(mini_server_->uuid(), resp.status().node_instance().permanent_uuid());

  // Regression test for KUDU-2148: try to get the status as the server is
  // starting. To surface this more frequently, we restart the server a number
  // of times.
  CountDownLatch latch(1);
  thread status_thread([&](){
    server::GetStatusRequestPB req;
    server::GetStatusResponsePB resp;
    RpcController controller;
    while (latch.count() > 0) {
      controller.Reset();
      resp.Clear();
      Status s = generic_proxy_->GetStatus(req, &resp, &controller);
      if (s.ok()) {
        // These two fields are guaranteed even if the request yielded an error.
        CHECK(resp.has_status());
        CHECK(resp.status().has_node_instance());
        if (resp.has_error()) {
          // But this one isn't set if the request yielded an error.
          CHECK(!resp.status().has_version_info());
        }
      }
    }
  });
  SCOPED_CLEANUP({
    status_thread.join();
  });

  // Can't safely restart unless we allow the replica to be destroyed.
  tablet_replica_.reset();

  for (int i = 0; i < (AllowSlowTests() ? 100 : 10); i++) {
    mini_server_->Shutdown();
    ASSERT_OK(mini_server_->Restart());
  }

  // All done, stop the test thread.
  latch.CountDown();
}

TEST_F(TabletServerTest, TestServerClock) {
  server::ServerClockRequestPB req;
  server::ServerClockResponsePB resp;
  RpcController controller;

  ASSERT_OK(generic_proxy_->ServerClock(req, &resp, &controller));
  ASSERT_GT(mini_server_->server()->clock()->Now().ToUint64(), resp.timestamp());
}

TEST_F(TabletServerTest, TestSetFlags) {
  server::GenericServiceProxy proxy(
      client_messenger_, mini_server_->bound_rpc_addr(),
      mini_server_->bound_rpc_addr().host());

  server::SetFlagRequestPB req;
  server::SetFlagResponsePB resp;

  // Set an invalid flag.
  {
    RpcController controller;
    req.set_flag("foo");
    req.set_value("bar");
    ASSERT_OK(proxy.SetFlag(req, &resp, &controller));
    SCOPED_TRACE(SecureDebugString(resp));
    EXPECT_EQ(server::SetFlagResponsePB::NO_SUCH_FLAG, resp.result());
    EXPECT_TRUE(resp.msg().empty());
  }

  // Set a valid flag to a valid value.
  {
    int32_t old_val = FLAGS_metrics_retirement_age_ms;
    RpcController controller;
    req.set_flag("metrics_retirement_age_ms");
    req.set_value("12345");
    ASSERT_OK(proxy.SetFlag(req, &resp, &controller));
    SCOPED_TRACE(SecureDebugString(resp));
    EXPECT_EQ(server::SetFlagResponsePB::SUCCESS, resp.result());
    EXPECT_EQ(resp.msg(), "metrics_retirement_age_ms set to 12345\n");
    EXPECT_EQ(Substitute("$0", old_val), resp.old_value());
    EXPECT_EQ(12345, FLAGS_metrics_retirement_age_ms);
  }

  // Set a valid flag to an invalid value.
  {
    RpcController controller;
    req.set_flag("metrics_retirement_age_ms");
    req.set_value("foo");
    ASSERT_OK(proxy.SetFlag(req, &resp, &controller));
    SCOPED_TRACE(SecureDebugString(resp));
    EXPECT_EQ(server::SetFlagResponsePB::BAD_VALUE, resp.result());
    EXPECT_EQ(resp.msg(), "Unable to set flag: bad value");
    EXPECT_EQ(12345, FLAGS_metrics_retirement_age_ms);
  }

  // Try setting a flag which isn't runtime-modifiable
  {
    RpcController controller;
    req.set_flag("tablet_bloom_target_fp_rate");
    req.set_value("1.0");
    ASSERT_OK(proxy.SetFlag(req, &resp, &controller));
    SCOPED_TRACE(SecureDebugString(resp));
    EXPECT_EQ(server::SetFlagResponsePB::NOT_SAFE, resp.result());
  }

  // Try again, but with the force flag.
  {
    RpcController controller;
    req.set_flag("tablet_bloom_target_fp_rate");
    req.set_value("1.0");
    req.set_force(true);
    ASSERT_OK(proxy.SetFlag(req, &resp, &controller));
    SCOPED_TRACE(SecureDebugString(resp));
    EXPECT_EQ(server::SetFlagResponsePB::SUCCESS, resp.result());
  }
}

TEST_F(TabletServerTest, TestWebPages) {
  EasyCurl c;
  faststring buf;
  string addr = mini_server_->bound_http_addr().ToString();

  // Tablets page should list tablet.
  ASSERT_OK(c.FetchURL(Substitute("http://$0/tablets", addr),
                              &buf));
  ASSERT_STR_CONTAINS(buf.ToString(), kTabletId);
  ASSERT_STR_CONTAINS(buf.ToString(), "RANGE (key) PARTITION UNBOUNDED</td>");

  // Tablet page should include the schema.
  ASSERT_OK(c.FetchURL(Substitute("http://$0/tablet?id=$1", addr, kTabletId),
                       &buf));
  ASSERT_STR_CONTAINS(buf.ToString(), "<th><u>key</u></th>");
  ASSERT_STR_CONTAINS(buf.ToString(), "<td>string NULLABLE</td>");

  // Test fetching metrics.
  // Fetching metrics has the side effect of retiring metrics, but not in a single pass.
  // So, we check a couple of times in a loop -- thus, if we had a bug where one of these
  // metrics was accidentally un-referenced too early, we'd cause it to get retired.
  // If the metrics survive several passes of fetching, then we are pretty sure they will
  // stick around properly for the whole lifetime of the server.
  FLAGS_metrics_retirement_age_ms = 0;
  for (int i = 0; i < 3; i++) {
    SCOPED_TRACE(i);
    ASSERT_OK(c.FetchURL(strings::Substitute("http://$0/jsonmetricz", addr), &buf));

    // Check that the tablet entry shows up.
    ASSERT_STR_CONTAINS(buf.ToString(), "\"type\": \"tablet\"");
    ASSERT_STR_CONTAINS(buf.ToString(), "\"id\": \"TestTablet\"");
    ASSERT_STR_CONTAINS(buf.ToString(), "\"partition\": \"RANGE (key) PARTITION UNBOUNDED");


    // Check entity attributes.
    ASSERT_STR_CONTAINS(buf.ToString(), "\"table_name\": \"TestTable\"");
    ASSERT_STR_CONTAINS(buf.ToString(), "\"table_id\": \"TestTable\"");

    // Check for the existence of some particular metrics for which we've had early-retirement
    // bugs in the past.
    ASSERT_STR_CONTAINS(buf.ToString(), "hybrid_clock_timestamp");
    ASSERT_STR_CONTAINS(buf.ToString(), "active_scanners");
    ASSERT_STR_CONTAINS(buf.ToString(), "threads_started");
    ASSERT_STR_CONTAINS(buf.ToString(), "code_cache_queries");
#ifdef TCMALLOC_ENABLED
    ASSERT_STR_CONTAINS(buf.ToString(), "tcmalloc_max_total_thread_cache_bytes");
#endif
    ASSERT_STR_CONTAINS(buf.ToString(), "glog_info_messages");
  }

  // Smoke-test the tracing infrastructure.
  ASSERT_OK(c.FetchURL(
                Substitute("http://$0/tracing/json/get_buffer_percent_full", addr, kTabletId),
                &buf));
  ASSERT_EQ(buf.ToString(), "0");

  string enable_req_json = "{\"categoryFilter\":\"*\", \"useContinuousTracing\": \"true\","
    " \"useSampling\": \"false\"}";
  string req_b64;
  strings::Base64Escape(enable_req_json, &req_b64);

  for (bool compressed : {false, true}) {
    ASSERT_OK(c.FetchURL(Substitute("http://$0/tracing/json/begin_recording?$1",
                                    addr,
                                    req_b64), &buf));
    ASSERT_EQ(buf.ToString(), "");
    ASSERT_OK(c.FetchURL(Substitute("http://$0/tracing/json/end_recording$1", addr,
                                    compressed ? "_compressed" : ""),
                         &buf));
    string json;
    if (compressed) {
      std::ostringstream ss;
      ASSERT_OK(zlib::Uncompress(buf, &ss));
      json = ss.str();
    } else {
      json = buf.ToString();
    }

    ASSERT_STR_CONTAINS(json, "__metadata");
  }

  ASSERT_OK(c.FetchURL(Substitute("http://$0/tracing/json/categories", addr),
                       &buf));
  ASSERT_STR_CONTAINS(buf.ToString(), "\"log\"");

  // Smoke test the pprof contention profiler handler.
  ASSERT_OK(c.FetchURL(Substitute("http://$0/pprof/contention?seconds=1", addr),
                       &buf));
  ASSERT_STR_CONTAINS(buf.ToString(), "discarded samples = 0");
#if defined(__linux__)
  // The executable name appears as part of the dump of /proc/self/maps, which
  // only exists on Linux.
  ASSERT_STR_CONTAINS(buf.ToString(), "tablet_server-test");
#endif
}

// Ensure that when a replica is in a failed / shutdown state, it returns an
// error for ConsensusState() requests.
TEST_F(TabletServerTest, TestFailedTabletsRejectConsensusState) {
  scoped_refptr<TabletReplica> replica;
  TSTabletManager* tablet_manager = mini_server_->server()->tablet_manager();
  ASSERT_TRUE(tablet_manager->LookupTablet(kTabletId, &replica));
  replica->SetError(Status::IOError("This error will leave the replica FAILED state at shutdown"));
  replica->Shutdown();
  ASSERT_EQ(tablet::FAILED, replica->state());

  auto consensus = replica->shared_consensus();
  ASSERT_TRUE(consensus);
  ConsensusStatePB cstate;
  Status s = consensus->ConsensusState(&cstate);
  ASSERT_TRUE(s.IsIllegalState()) << s.ToString();
  ASSERT_STR_CONTAINS(s.ToString(), "Tablet replica is shutdown");
}

// Test that tablets that get failed and deleted will eventually show up as
// failed tombstones on the web UI.
TEST_F(TabletServerTest, TestFailedTabletsOnWebUI) {
  scoped_refptr<TabletReplica> replica;
  TSTabletManager* tablet_manager = mini_server_->server()->tablet_manager();
  ASSERT_TRUE(tablet_manager->LookupTablet(kTabletId, &replica));
  replica->SetError(Status::IOError("This error will leave the replica FAILED state at shutdown"));
  replica->Shutdown();
  ASSERT_EQ(tablet::FAILED, replica->state());

  // Now delete the tablet and leave it tombstoned, e.g. as if the failed
  // replica were deleted.
  TabletServerErrorPB::Code error_code;
  ASSERT_OK(tablet_manager->DeleteTablet(kTabletId,
      tablet::TABLET_DATA_TOMBSTONED, boost::none, &error_code));

  EasyCurl c;
  faststring buf;
  const string addr = mini_server_->bound_http_addr().ToString();
  ASSERT_OK(c.FetchURL(Substitute("http://$0/tablets", addr), &buf));

  // Ensure the html contains the "Tombstoned Tablets" header, indicating the
  // failed tombstone is correctly displayed as a tombstone.
  ASSERT_STR_CONTAINS(buf.ToString(), "Tombstoned Tablets");
}

// Test that tombstoned tablets are displayed correctly in the web ui:
// - After restart, status message of "Tombstoned" instead of "Tablet initializing...".
// - No consensus configuration.
TEST_F(TabletServerTest, TestTombstonedTabletOnWebUI) {
  TSTabletManager* tablet_manager = mini_server_->server()->tablet_manager();
  TabletServerErrorPB::Code error_code;
  ASSERT_OK(tablet_manager->DeleteTablet(kTabletId,
                                         tablet::TABLET_DATA_TOMBSTONED, boost::none, &error_code));

  // Restart the server. We drop the tablet_replica_ reference since it becomes
  // invalid when the server shuts down.
  tablet_replica_.reset();
  mini_server_->Shutdown();
  ASSERT_OK(mini_server_->Restart());
  ASSERT_OK(mini_server_->WaitStarted());

  EasyCurl c;
  faststring buf;
  const string addr = mini_server_->bound_http_addr().ToString();
  ASSERT_OK(c.FetchURL(Substitute("http://$0/tablets", addr), &buf));

  // Ensure the html contains the "Tombstoned Tablets" header and
  // a table entry with the proper status message.
  string s = buf.ToString();
  ASSERT_STR_CONTAINS(s, "Tombstoned Tablets");
  ASSERT_STR_CONTAINS(s, "<td>Tombstoned</td>");
  ASSERT_STR_NOT_CONTAINS(s, "<td>Tablet initializing...</td>");

  // Since the consensus config shouldn't be displayed, the html should not
  // contain the server's RPC address.
  ASSERT_STR_NOT_CONTAINS(s, mini_server_->bound_rpc_addr().ToString());
}

class TabletServerDiskFailureTest : public TabletServerTestBase {
 public:
  virtual void SetUp() override {
    const int kNumDirs = 5;
    NO_FATALS(TabletServerTestBase::SetUp());
    // Ensure the server will flush frequently.
    FLAGS_enable_maintenance_manager = true;
    FLAGS_maintenance_manager_num_threads = kNumDirs;
    FLAGS_flush_threshold_mb = 1;
    FLAGS_flush_threshold_secs = 1;

    // Create a brand new tablet server with multiple disks, ensuring it can
    // survive at least one disk failure.
    NO_FATALS(StartTabletServer(/*num_data_dirs=*/ kNumDirs));
  }
};

// Test that applies random operations to a tablet with a non-zero disk-failure
// injection rate.
TEST_F(TabletServerDiskFailureTest, TestRandomOpSequence) {
  if (!AllowSlowTests()) {
    LOG(INFO) << "Not running slow test. To run, use KUDU_ALLOW_SLOW_TESTS=1";
    return;
  }
  typedef vector<RowOperationsPB::Type> OpTypeList;
  const OpTypeList kOpsIfKeyNotPresent = { RowOperationsPB::INSERT, RowOperationsPB::UPSERT };
  const OpTypeList kOpsIfKeyPresent = { RowOperationsPB::UPSERT, RowOperationsPB::UPDATE,
                                        RowOperationsPB::DELETE };
  const int kMaxKey = 100000;

  // Set these way up-front so we can change a single value to actually start
  // injecting errors. Inject errors into all data dirs but one.
  FLAGS_crash_on_eio = false;
  const vector<string> failed_dirs = { mini_server_->options()->fs_opts.data_roots.begin() + 1,
                                       mini_server_->options()->fs_opts.data_roots.end() };
  FLAGS_env_inject_eio_globs = JoinStrings(JoinPathSegmentsV(failed_dirs, "**"), ",");

  set<int> keys;
  const auto GetRandomString = [] {
    return StringPrintf("%d", rand() % kMaxKey);
  };

  // Perform a random op (insert, update, upsert, or delete).
  const auto PerformOp = [&] {
    // Set up the request.
    WriteRequestPB req;
    req.set_tablet_id(kTabletId);
    RETURN_NOT_OK(SchemaToPB(schema_, req.mutable_schema()));

    // Set up the other state.
    WriteResponsePB resp;
    RpcController controller;
    RowOperationsPB::Type op_type;
    int key = rand() % kMaxKey;
    auto key_iter = keys.find(key);
    if (key_iter == keys.end()) {
      // If the key already exists, insert or upsert.
      op_type = kOpsIfKeyNotPresent[rand() % kOpsIfKeyNotPresent.size()];
    } else {
      // ... else we can do anything but insert.
      op_type = kOpsIfKeyPresent[rand() % kOpsIfKeyPresent.size()];
    }

    // Add the op to the request.
    if (op_type != RowOperationsPB::DELETE) {
      AddTestRowToPB(op_type, schema_, key, key, GetRandomString(),
                     req.mutable_row_operations());
      keys.insert(key);
    } else {
      AddTestKeyToPB(RowOperationsPB::DELETE, schema_, key, req.mutable_row_operations());
      keys.erase(key_iter);
    }

    // Finally, write to the server and log the response.
    RETURN_NOT_OK_PREPEND(proxy_->Write(req, &resp, &controller), "Failed to write");
    LOG(INFO) << "Tablet server responded with: " << SecureDebugString(resp);
    return resp.has_error() ?  StatusFromPB(resp.error().status()) : Status::OK();
  };

  // Perform some arbitrarily large number of ops, with some pauses to encourage flushes.
  for (int i = 0; i < 500; i++) {
    if (i % 10) {
      SleepFor(MonoDelta::FromMilliseconds(100));
    }
    ASSERT_OK(PerformOp());
  }
  // At this point, a bunch of operations have gone through successfully. Fail
  // one of the disks that the tablet lives on.
  FLAGS_env_inject_eio = 0.01;

  // The tablet will eventually be failed and will not be able to accept
  // updates. Keep on inserting until that happens.
  ASSERT_EVENTUALLY([&] {
    Status s;
    for (int i = 0; i < 150 && s.ok(); i++) {
      s = PerformOp();
    }
    ASSERT_FALSE(s.ok());
  });
  LOG(INFO) << "Failure was caught by an op!";
  ASSERT_EVENTUALLY([&] {
    ASSERT_EQ(tablet::FAILED, tablet_replica_->state());
  });
  LOG(INFO) << "Tablet was successfully failed";
}

TEST_F(TabletServerTest, TestInsert) {
  WriteRequestPB req;

  req.set_tablet_id(kTabletId);

  WriteResponsePB resp;
  RpcController controller;

  scoped_refptr<TabletReplica> tablet;
  ASSERT_TRUE(mini_server_->server()->tablet_manager()->LookupTablet(kTabletId, &tablet));
  scoped_refptr<Counter> rows_inserted =
    METRIC_rows_inserted.Instantiate(tablet->tablet()->GetMetricEntity());
  ASSERT_EQ(0, rows_inserted->value());
  tablet.reset();

  // Send a bad insert which has an empty schema. This should result
  // in an error.
  {
    AddTestRowToPB(RowOperationsPB::INSERT, schema_, 1234, 5678, "hello world via RPC",
                   req.mutable_row_operations());

    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_TRUE(resp.has_error());
    ASSERT_EQ(TabletServerErrorPB::MISMATCHED_SCHEMA, resp.error().code());
    Status s = StatusFromPB(resp.error().status());
    EXPECT_TRUE(s.IsInvalidArgument());
    ASSERT_STR_CONTAINS(s.ToString(),
                        "Client missing required column: key[int32 NOT NULL]");
    req.clear_row_operations();
  }

  // Send an empty request with the correct schema.
  // This should succeed and do nothing.
  {
    controller.Reset();
    ASSERT_OK(SchemaToPB(schema_, req.mutable_schema()));
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_FALSE(resp.has_error());
    req.clear_row_operations();
  }

  // Send an actual row insert.
  {
    controller.Reset();
    RowOperationsPB* data = req.mutable_row_operations();
    data->Clear();

    AddTestRowToPB(RowOperationsPB::INSERT, schema_, 1234, 5678,
                   "hello world via RPC", data);
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_FALSE(resp.has_error());
    req.clear_row_operations();
    ASSERT_EQ(1, rows_inserted->value());
  }

  // Send a batch with multiple rows, one of which is a duplicate of
  // the above insert, and one of which has a too-large value.
  // This should generate two errors into per_row_errors.
  {
    const string kTooLargeValue(100 * 1024, 'x');
    controller.Reset();
    RowOperationsPB* data = req.mutable_row_operations();
    data->Clear();

    AddTestRowToPB(RowOperationsPB::INSERT, schema_, 1, 1, "ceci n'est pas une dupe", data);
    AddTestRowToPB(RowOperationsPB::INSERT, schema_, 2, 1, "also not a dupe key", data);
    AddTestRowToPB(RowOperationsPB::INSERT, schema_, 1234, 1, "I am a duplicate key", data);
    AddTestRowToPB(RowOperationsPB::INSERT, schema_, 3, 1, kTooLargeValue, data);
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_FALSE(resp.has_error()) << SecureShortDebugString(resp);
    ASSERT_EQ(3, rows_inserted->value());  // This counter only counts successful inserts.
    ASSERT_EQ(2, resp.per_row_errors().size());

    // Check the duplicate key error.
    ASSERT_EQ(2, resp.per_row_errors().Get(0).row_index());
    Status s = StatusFromPB(resp.per_row_errors().Get(0).error());
    ASSERT_STR_CONTAINS(s.ToString(), "Already present");

    // Check the value-too-large error.
    ASSERT_EQ(3, resp.per_row_errors().Get(1).row_index());
    s = StatusFromPB(resp.per_row_errors().Get(1).error());
    ASSERT_STR_CONTAINS(s.ToString(), "Invalid argument");
  }

  // get the clock's current timestamp
  Timestamp now_before = mini_server_->server()->clock()->Now();

  rows_inserted = nullptr;
  ASSERT_NO_FATAL_FAILURE(ShutdownAndRebuildTablet());
  VerifyRows(schema_, { KeyValue(1, 1), KeyValue(2, 1), KeyValue(1234, 5678) });

  // get the clock's timestamp after replay
  Timestamp now_after = mini_server_->server()->clock()->Now();

  // make sure 'now_after' is greater than or equal to 'now_before'
  ASSERT_GE(now_after.value(), now_before.value());
}

TEST_F(TabletServerTest, TestExternalConsistencyModes_ClientPropagated) {
  WriteRequestPB req;
  req.set_tablet_id(kTabletId);
  WriteResponsePB resp;
  RpcController controller;

  scoped_refptr<TabletReplica> tablet;
  ASSERT_TRUE(
      mini_server_->server()->tablet_manager()->LookupTablet(kTabletId,
                                                             &tablet));
  scoped_refptr<Counter> rows_inserted =
      METRIC_rows_inserted.Instantiate(tablet->tablet()->GetMetricEntity());
  ASSERT_EQ(0, rows_inserted->value());

  // get the current time
  Timestamp current = mini_server_->server()->clock()->Now();
  // advance current to some time in the future. we do 5 secs to make
  // sure this timestamp will still be in the future when it reaches the
  // server.
  current = HybridClock::TimestampFromMicroseconds(
      HybridClock::GetPhysicalValueMicros(current) + 5000000);

  // Send an actual row insert.
  ASSERT_OK(SchemaToPB(schema_, req.mutable_schema()));
  AddTestRowToPB(RowOperationsPB::INSERT, schema_, 1234, 5678, "hello world via RPC",
                 req.mutable_row_operations());

  // set the external consistency mode and the timestamp
  req.set_external_consistency_mode(CLIENT_PROPAGATED);

  req.set_propagated_timestamp(current.ToUint64());
  SCOPED_TRACE(SecureDebugString(req));
  ASSERT_OK(proxy_->Write(req, &resp, &controller));
  SCOPED_TRACE(SecureDebugString(resp));
  ASSERT_FALSE(resp.has_error());
  req.clear_row_operations();
  ASSERT_EQ(1, rows_inserted->value());

  // make sure the server returned a write timestamp where only
  // the logical value was increased since he should have updated
  // its clock with the client's value.
  Timestamp write_timestamp(resp.timestamp());

  ASSERT_EQ(HybridClock::GetPhysicalValueMicros(current),
            HybridClock::GetPhysicalValueMicros(write_timestamp));

  ASSERT_EQ(HybridClock::GetLogicalValue(current) + 1,
            HybridClock::GetLogicalValue(write_timestamp));
}

TEST_F(TabletServerTest, TestExternalConsistencyModes_CommitWait) {
  WriteRequestPB req;
  req.set_tablet_id(kTabletId);
  WriteResponsePB resp;
  RpcController controller;
  HybridClock* hclock = down_cast<HybridClock*, Clock>(mini_server_->server()->clock());

  scoped_refptr<TabletReplica> tablet;
  ASSERT_TRUE(
      mini_server_->server()->tablet_manager()->LookupTablet(kTabletId,
                                                             &tablet));
  scoped_refptr<Counter> rows_inserted =
      METRIC_rows_inserted.Instantiate(
          tablet->tablet()->GetMetricEntity());
  ASSERT_EQ(0, rows_inserted->value());

  // get current time, with and without error
  Timestamp now_before;
  uint64_t error_before;
  hclock->NowWithError(&now_before, &error_before);

  uint64_t now_before_usec = HybridClock::GetPhysicalValueMicros(now_before);
  LOG(INFO) << "Submitting write with commit wait at: " << now_before_usec << " us +- "
      << error_before << " us";

  // Send an actual row insert.
  ASSERT_OK(SchemaToPB(schema_, req.mutable_schema()));
  AddTestRowToPB(RowOperationsPB::INSERT, schema_, 1234, 5678, "hello world via RPC",
                 req.mutable_row_operations());

  // set the external consistency mode to COMMIT_WAIT
  req.set_external_consistency_mode(COMMIT_WAIT);

  SCOPED_TRACE(SecureDebugString(req));
  ASSERT_OK(proxy_->Write(req, &resp, &controller));
  SCOPED_TRACE(SecureDebugString(resp));
  ASSERT_FALSE(resp.has_error());
  req.clear_row_operations();
  ASSERT_EQ(1, rows_inserted->value());

  // Two things must have happened.
  // 1 - The write timestamp must be greater than 'now_before'
  // 2 - The write must have taken at least 'error_before' to complete (two
  //     times more in average).

  Timestamp now_after;
  uint64_t error_after;
  hclock->NowWithError(&now_after, &error_after);

  Timestamp write_timestamp(resp.timestamp());

  uint64_t write_took = HybridClock::GetPhysicalValueMicros(now_after) -
      HybridClock::GetPhysicalValueMicros(now_before);

  LOG(INFO) << "Write applied at: " << HybridClock::GetPhysicalValueMicros(write_timestamp)
      << " us, current time: " << HybridClock::GetPhysicalValueMicros(now_after)
      << " us, write took: " << write_took << " us";

  ASSERT_GT(write_timestamp.value(), now_before.value());

  // see HybridClockTest.TestWaitUntilAfter_TestCase2
  if (error_after >= error_before) {
    ASSERT_GE(write_took, 2 * error_before);
  } else {
    ASSERT_GE(write_took, error_before);
  }
}


TEST_F(TabletServerTest, TestInsertAndMutate) {

  scoped_refptr<TabletReplica> tablet;
  ASSERT_TRUE(mini_server_->server()->tablet_manager()->LookupTablet(kTabletId, &tablet));
  scoped_refptr<Counter> rows_inserted =
      METRIC_rows_inserted.Instantiate(tablet->tablet()->GetMetricEntity());
  scoped_refptr<Counter> rows_updated =
      METRIC_rows_updated.Instantiate(tablet->tablet()->GetMetricEntity());
  scoped_refptr<Counter> rows_deleted =
      METRIC_rows_deleted.Instantiate(tablet->tablet()->GetMetricEntity());
  ASSERT_EQ(0, rows_inserted->value());
  ASSERT_EQ(0, rows_updated->value());
  ASSERT_EQ(0, rows_deleted->value());
  tablet.reset();

  RpcController controller;

  {
    WriteRequestPB req;
    WriteResponsePB resp;
    req.set_tablet_id(kTabletId);
    RowOperationsPB* data = req.mutable_row_operations();
    ASSERT_OK(SchemaToPB(schema_, req.mutable_schema()));

    AddTestRowToPB(RowOperationsPB::INSERT, schema_, 1, 1, "original1", data);
    AddTestRowToPB(RowOperationsPB::INSERT, schema_, 2, 2, "original2", data);
    AddTestRowToPB(RowOperationsPB::INSERT, schema_, 3, 3, "original3", data);
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_FALSE(resp.has_error()) << SecureShortDebugString(resp);
    ASSERT_EQ(0, resp.per_row_errors().size());
    ASSERT_EQ(3, rows_inserted->value());
    ASSERT_EQ(0, rows_updated->value());
    controller.Reset();
  }

  // Try and mutate the rows inserted above
  {
    WriteRequestPB req;
    WriteResponsePB resp;
    req.set_tablet_id(kTabletId);
    ASSERT_OK(SchemaToPB(schema_, req.mutable_schema()));

    AddTestRowToPB(RowOperationsPB::UPDATE, schema_, 1, 2, "mutation1",
                   req.mutable_row_operations());
    AddTestRowToPB(RowOperationsPB::UPDATE, schema_, 2, 3, "mutation2",
                   req.mutable_row_operations());
    AddTestRowToPB(RowOperationsPB::UPDATE, schema_, 3, 4, "mutation3",
                   req.mutable_row_operations());
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_FALSE(resp.has_error()) << SecureShortDebugString(resp);
    ASSERT_EQ(0, resp.per_row_errors().size());
    ASSERT_EQ(3, rows_inserted->value());
    ASSERT_EQ(3, rows_updated->value());
    controller.Reset();
  }

  // Try and mutate a non existent row key (should get an error)
  {
    WriteRequestPB req;
    WriteResponsePB resp;
    req.set_tablet_id(kTabletId);
    ASSERT_OK(SchemaToPB(schema_, req.mutable_schema()));

    AddTestRowToPB(RowOperationsPB::UPDATE, schema_, 1234, 2, "mutated",
                   req.mutable_row_operations());
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_FALSE(resp.has_error()) << SecureShortDebugString(resp);
    ASSERT_EQ(1, resp.per_row_errors().size());
    ASSERT_EQ(3, rows_updated->value());
    controller.Reset();
  }

  // Try and delete 1 row
  {
    WriteRequestPB req;
    WriteResponsePB resp;
    req.set_tablet_id(kTabletId);
    ASSERT_OK(SchemaToPB(schema_, req.mutable_schema()));

    AddTestKeyToPB(RowOperationsPB::DELETE, schema_, 1, req.mutable_row_operations());
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_FALSE(resp.has_error())<< SecureShortDebugString(resp);
    ASSERT_EQ(0, resp.per_row_errors().size());
    ASSERT_EQ(3, rows_updated->value());
    ASSERT_EQ(1, rows_deleted->value());
    controller.Reset();
  }

  // Now try and mutate a row we just deleted, we should get an error
  {
    WriteRequestPB req;
    WriteResponsePB resp;
    req.set_tablet_id(kTabletId);
    ASSERT_OK(SchemaToPB(schema_, req.mutable_schema()));

    AddTestRowToPB(RowOperationsPB::UPDATE, schema_, 1, 2, "mutated1",
                   req.mutable_row_operations());
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_FALSE(resp.has_error())<< SecureShortDebugString(resp);
    ASSERT_EQ(1, resp.per_row_errors().size());
    controller.Reset();
  }

  ASSERT_EQ(3, rows_inserted->value());
  ASSERT_EQ(3, rows_updated->value());

  // At this point, we have two rows left (row key 2 and 3).
  VerifyRows(schema_, { KeyValue(2, 3), KeyValue(3, 4) });

  // Do a mixed operation (some insert, update, and delete, some of which fail)
  {
    const string kTooLargeValue(100 * 1024, 'x');
    WriteRequestPB req;
    WriteResponsePB resp;
    req.set_tablet_id(kTabletId);
    ASSERT_OK(SchemaToPB(schema_, req.mutable_schema()));

    RowOperationsPB* ops = req.mutable_row_operations();
    // op 0: Mutate row 1, which doesn't exist. This should fail.
    AddTestRowToPB(RowOperationsPB::UPDATE, schema_, 1, 3, "mutate_should_fail", ops);
    // op 1: Insert a new row 4 (succeeds)
    AddTestRowToPB(RowOperationsPB::INSERT, schema_, 4, 4, "new row 4", ops);
    // op 2: Delete a non-existent row 5 (should fail)
    AddTestKeyToPB(RowOperationsPB::DELETE, schema_, 5, ops);
    // op 3: Insert a new row 6 (succeeds)
    AddTestRowToPB(RowOperationsPB::INSERT, schema_, 6, 6, "new row 6", ops);
    // op 4: update a row with a too-large value (fail)
    AddTestRowToPB(RowOperationsPB::UPDATE, schema_, 4, 6, kTooLargeValue, ops);

    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_FALSE(resp.has_error())<< SecureShortDebugString(resp);
    ASSERT_EQ(3, resp.per_row_errors().size());
    EXPECT_EQ("row_index: 0 error { code: NOT_FOUND message: \"key not found\" }",
              SecureShortDebugString(resp.per_row_errors(0)));
    EXPECT_EQ("row_index: 2 error { code: NOT_FOUND message: \"key not found\" }",
              SecureShortDebugString(resp.per_row_errors(1)));
    EXPECT_EQ("row_index: 4 error { code: INVALID_ARGUMENT message: "
              "\"value too large for column \\'string_val\\' (102400 bytes, "
              "maximum is 65536 bytes)\" }",
              SecureShortDebugString(resp.per_row_errors(2)));
    controller.Reset();
  }

  // get the clock's current timestamp
  Timestamp now_before = mini_server_->server()->clock()->Now();

  rows_inserted = nullptr;
  rows_updated = nullptr;
  ASSERT_NO_FATAL_FAILURE(ShutdownAndRebuildTablet());
  VerifyRows(schema_, { KeyValue(2, 3), KeyValue(3, 4), KeyValue(4, 4), KeyValue(6, 6) });

  // get the clock's timestamp after replay
  Timestamp now_after = mini_server_->server()->clock()->Now();

  // make sure 'now_after' is greater that or equal to 'now_before'
  ASSERT_GE(now_after.value(), now_before.value());
}

// Test that passing a schema with fields not present in the tablet schema
// throws an exception.
TEST_F(TabletServerTest, TestInvalidWriteRequest_BadSchema) {
  SchemaBuilder schema_builder(schema_);
  ASSERT_OK(schema_builder.AddColumn("col_doesnt_exist", INT32));
  Schema bad_schema_with_ids = schema_builder.Build();
  Schema bad_schema = schema_builder.BuildWithoutIds();

  // Send a row insert with an extra column
  {
    WriteRequestPB req;
    WriteResponsePB resp;
    RpcController controller;

    req.set_tablet_id(kTabletId);
    RowOperationsPB* data = req.mutable_row_operations();
    ASSERT_OK(SchemaToPB(bad_schema, req.mutable_schema()));

    KuduPartialRow row(&bad_schema);
    CHECK_OK(row.SetInt32("key", 1234));
    CHECK_OK(row.SetInt32("int_val", 5678));
    CHECK_OK(row.SetStringCopy("string_val", "hello world via RPC"));
    CHECK_OK(row.SetInt32("col_doesnt_exist", 91011));
    RowOperationsPBEncoder enc(data);
    enc.Add(RowOperationsPB::INSERT, row);

    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_TRUE(resp.has_error());
    ASSERT_EQ(TabletServerErrorPB::MISMATCHED_SCHEMA, resp.error().code());
    ASSERT_STR_CONTAINS(resp.error().status().message(),
                        "Client provided column col_doesnt_exist[int32 NOT NULL]"
                        " not present in tablet");
  }

  // Send a row mutation with an extra column and IDs
  {
    WriteRequestPB req;
    WriteResponsePB resp;
    RpcController controller;

    req.set_tablet_id(kTabletId);
    ASSERT_OK(SchemaToPB(bad_schema_with_ids, req.mutable_schema()));

    AddTestKeyToPB(RowOperationsPB::UPDATE, bad_schema_with_ids, 1,
                   req.mutable_row_operations());
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_TRUE(resp.has_error());
    ASSERT_EQ(TabletServerErrorPB::INVALID_SCHEMA, resp.error().code());
    ASSERT_STR_CONTAINS(resp.error().status().message(),
                        "User requests should not have Column IDs");
  }
}

// Executes mutations each time a Tablet goes through a compaction/flush
// lifecycle hook. This allows to create mutations of all possible types
// deterministically. The purpose is to make sure such mutations are replayed
// correctly on tablet bootstrap.
class MyCommonHooks : public Tablet::FlushCompactCommonHooks,
                      public Tablet::FlushFaultHooks,
                      public Tablet::CompactionFaultHooks {
 public:
  explicit MyCommonHooks(TabletServerTest* test)
  : test_(test),
    iteration_(0) {}

  Status DoHook(int32_t key, int32_t new_int_val) {
    test_->UpdateTestRowRemote(key, new_int_val);
    return Status::OK();
  }

  // This should go in pre-flush and get flushed
  virtual Status PostSwapNewMemRowSet() OVERRIDE {
    return DoHook(1, 10 + iteration_);
  }
  // This should go in after the flush, but before
  // the duplicating row set, i.e., this should appear as
  // a missed delta.
  virtual Status PostTakeMvccSnapshot() OVERRIDE {
    return DoHook(2, 20 + iteration_);
  }
  // This too should appear as a missed delta.
  virtual Status PostWriteSnapshot() OVERRIDE {
    return DoHook(3, 30 + iteration_);
  }
  // This should appear as a duplicated mutation
  virtual Status PostSwapInDuplicatingRowSet() OVERRIDE {
    return DoHook(4, 40 + iteration_);
  }
  // This too should appear as a duplicated mutation
  virtual Status PostReupdateMissedDeltas() OVERRIDE {
    return DoHook(5, 50 + iteration_);
  }
  // This should go into the new delta.
  virtual Status PostSwapNewRowSet() OVERRIDE {
    return DoHook(6, 60 + iteration_);
  }
  // This should go in pre-flush (only on compactions)
  virtual Status PostSelectIterators() OVERRIDE {
    return DoHook(7, 70 + iteration_);
  }
  void increment_iteration() {
    iteration_++;
  }
 protected:
  TabletServerTest* test_;
  int iteration_;
};

// Tests performing mutations that are going to the initial MRS
// or to a DMS, when the MRS is flushed. This also tests that the
// log produced on recovery allows to re-recover the original state.
TEST_F(TabletServerTest, TestRecoveryWithMutationsWhileFlushing) {

  InsertTestRowsRemote(1, 7);

  shared_ptr<MyCommonHooks> hooks(new MyCommonHooks(this));

  tablet_replica_->tablet()->SetFlushHooksForTests(hooks);
  tablet_replica_->tablet()->SetCompactionHooksForTests(hooks);
  tablet_replica_->tablet()->SetFlushCompactCommonHooksForTests(hooks);

  ASSERT_OK(tablet_replica_->tablet()->Flush());

  // Shutdown the tserver and try and rebuild the tablet from the log
  // produced on recovery (recovery flushed no state, but produced a new
  // log).
  ASSERT_NO_FATAL_FAILURE(ShutdownAndRebuildTablet());
  VerifyRows(schema_, { KeyValue(1, 10),
                        KeyValue(2, 20),
                        KeyValue(3, 30),
                        KeyValue(4, 40),
                        KeyValue(5, 50),
                        KeyValue(6, 60),
                        // the last hook only fires on compaction
                        // so this isn't mutated
                        KeyValue(7, 7) });

  // Shutdown and rebuild again to test that the log generated during
  // the previous recovery allows to perform recovery again.
  ASSERT_NO_FATAL_FAILURE(ShutdownAndRebuildTablet());
  VerifyRows(schema_, { KeyValue(1, 10),
                        KeyValue(2, 20),
                        KeyValue(3, 30),
                        KeyValue(4, 40),
                        KeyValue(5, 50),
                        KeyValue(6, 60),
                        KeyValue(7, 7) });
}

// Tests performing mutations that are going to a DMS or to the following
// DMS, when the initial one is flushed.
TEST_F(TabletServerTest, TestRecoveryWithMutationsWhileFlushingAndCompacting) {

  InsertTestRowsRemote(1, 7);

  shared_ptr<MyCommonHooks> hooks(new MyCommonHooks(this));

  tablet_replica_->tablet()->SetFlushHooksForTests(hooks);
  tablet_replica_->tablet()->SetCompactionHooksForTests(hooks);
  tablet_replica_->tablet()->SetFlushCompactCommonHooksForTests(hooks);

  // flush the first time
  ASSERT_OK(tablet_replica_->tablet()->Flush());

  ASSERT_NO_FATAL_FAILURE(ShutdownAndRebuildTablet());
  VerifyRows(schema_, { KeyValue(1, 10),
                        KeyValue(2, 20),
                        KeyValue(3, 30),
                        KeyValue(4, 40),
                        KeyValue(5, 50),
                        KeyValue(6, 60),
                        KeyValue(7, 7) });
  hooks->increment_iteration();

  // set the hooks on the new tablet
  tablet_replica_->tablet()->SetFlushHooksForTests(hooks);
  tablet_replica_->tablet()->SetCompactionHooksForTests(hooks);
  tablet_replica_->tablet()->SetFlushCompactCommonHooksForTests(hooks);

  // insert an additional row so that we can flush
  InsertTestRowsRemote(8, 1);

  // flush an additional MRS so that we have two DiskRowSets and then compact
  // them making sure that mutations executed mid compaction are replayed as
  // expected
  ASSERT_OK(tablet_replica_->tablet()->Flush());
  VerifyRows(schema_, { KeyValue(1, 11),
                        KeyValue(2, 21),
                        KeyValue(3, 31),
                        KeyValue(4, 41),
                        KeyValue(5, 51),
                        KeyValue(6, 61),
                        KeyValue(7, 7),
                        KeyValue(8, 8) });

  hooks->increment_iteration();
  ASSERT_OK(tablet_replica_->tablet()->Compact(Tablet::FORCE_COMPACT_ALL));

  // get the clock's current timestamp
  Timestamp now_before = mini_server_->server()->clock()->Now();

  // Shutdown the tserver and try and rebuild the tablet from the log
  // produced on recovery (recovery flushed no state, but produced a new
  // log).
  ASSERT_NO_FATAL_FAILURE(ShutdownAndRebuildTablet());
  VerifyRows(schema_, { KeyValue(1, 11),
                        KeyValue(2, 22),
                        KeyValue(3, 32),
                        KeyValue(4, 42),
                        KeyValue(5, 52),
                        KeyValue(6, 62),
                        KeyValue(7, 72),
                        KeyValue(8, 8) });

  // get the clock's timestamp after replay
  Timestamp now_after = mini_server_->server()->clock()->Now();

  // make sure 'now_after' is greater than or equal to 'now_before'
  ASSERT_GE(now_after.value(), now_before.value());
}

#define ANFF ASSERT_NO_FATAL_FAILURE

// Regression test for KUDU-176. Ensures that after a major delta compaction,
// restarting properly recovers the tablet.
TEST_F(TabletServerTest, TestKUDU_176_RecoveryAfterMajorDeltaCompaction) {

  // Flush a DRS with 1 rows.
  ASSERT_NO_FATAL_FAILURE(InsertTestRowsRemote(1, 1));
  ASSERT_OK(tablet_replica_->tablet()->Flush());
  ANFF(VerifyRows(schema_, { KeyValue(1, 1) }));

  // Update it, flush deltas.
  ANFF(UpdateTestRowRemote(1, 2));
  ASSERT_OK(tablet_replica_->tablet()->FlushBiggestDMS());
  ANFF(VerifyRows(schema_, { KeyValue(1, 2) }));

  // Major compact deltas.
  {
    vector<shared_ptr<tablet::RowSet> > rsets;
    tablet_replica_->tablet()->GetRowSetsForTests(&rsets);
    vector<ColumnId> col_ids = { tablet_replica_->tablet()->schema()->column_id(1),
                                 tablet_replica_->tablet()->schema()->column_id(2) };
    ASSERT_OK(tablet_replica_->tablet()->DoMajorDeltaCompaction(col_ids, rsets[0]))
  }

  // Verify that data is still the same.
  ANFF(VerifyRows(schema_, { KeyValue(1, 2) }));

  // Verify that data remains after a restart.
  ASSERT_OK(ShutdownAndRebuildTablet());
  ANFF(VerifyRows(schema_, { KeyValue(1, 2) }));
}

// Regression test for KUDU-1341, a case in which, during bootstrap,
// we have a DELETE for a row which is still live in multiple on-disk
// rowsets.
TEST_F(TabletServerTest, TestKUDU_1341) {
  for (int i = 0; i < 3; i++) {
    // Insert a row to DMS and flush it.
    ANFF(InsertTestRowsRemote(1, 1));
    ASSERT_OK(tablet_replica_->tablet()->Flush());

    // Update and delete row (in DMS)
    ANFF(UpdateTestRowRemote(1, i));
    ANFF(DeleteTestRowsRemote(1, 1));
  }

  // Insert row again, update it in MRS before flush, and
  // flush.
  ANFF(InsertTestRowsRemote(1, 1));
  ANFF(UpdateTestRowRemote(1, 12345));
  ASSERT_OK(tablet_replica_->tablet()->Flush());

  ANFF(VerifyRows(schema_, { KeyValue(1, 12345) }));

  // Test restart.
  ASSERT_OK(ShutdownAndRebuildTablet());
  ANFF(VerifyRows(schema_, { KeyValue(1, 12345) }));
  ASSERT_OK(tablet_replica_->tablet()->Flush());
  ANFF(VerifyRows(schema_, { KeyValue(1, 12345) }));

  // Test compaction after restart.
  ASSERT_OK(tablet_replica_->tablet()->Compact(Tablet::FORCE_COMPACT_ALL));
  ANFF(VerifyRows(schema_, { KeyValue(1, 12345) }));
}

TEST_F(TabletServerTest, TestExactlyOnceForErrorsAcrossRestart) {
  WriteRequestPB req;
  WriteResponsePB resp;
  RpcController rpc;

  // Set up a request to insert two rows.
  req.set_tablet_id(kTabletId);
  AddTestRowToPB(RowOperationsPB::INSERT, schema_, 1234, 5678, "hello world via RPC",
                 req.mutable_row_operations());
  AddTestRowToPB(RowOperationsPB::INSERT, schema_, 12345, 5679, "hello world via RPC2",
                 req.mutable_row_operations());
  ASSERT_OK(SchemaToPB(schema_, req.mutable_schema()));

  // Insert it, assuming no errors.
  {
    SCOPED_TRACE(req.DebugString());
    ASSERT_OK(proxy_->Write(req, &resp, &rpc));
    SCOPED_TRACE(resp.DebugString());
    ASSERT_FALSE(resp.has_error());
    ASSERT_EQ(0, resp.per_row_errors_size());
  }

  // Set up a RequestID to use in the later requests.
  rpc::RequestIdPB req_id;
  req_id.set_client_id("client-id");
  req_id.set_seq_no(1);
  req_id.set_first_incomplete_seq_no(1);
  req_id.set_attempt_no(1);

  // Insert the row again, with the request ID specified. We should expect an
  // "ALREADY_PRESENT" error.
  {
    rpc.Reset();
    rpc.SetRequestIdPB(unique_ptr<rpc::RequestIdPB>(new rpc::RequestIdPB(req_id)));
    ASSERT_OK(proxy_->Write(req, &resp, &rpc));
    SCOPED_TRACE(resp.DebugString());
    ASSERT_FALSE(resp.has_error());
    ASSERT_EQ(2, resp.per_row_errors_size());
  }

  // Restart the tablet server several times, and after each restart, send a new attempt of the
  // same request. We make the request itself invalid by clearing the schema and ops, but
  // that shouldn't matter since it's just hitting the ResultTracker and returning the
  // cached response. If the ResultTracker didn't have a cached response, then we'd get an
  // error about an invalid request.
  req.clear_schema();
  req.clear_row_operations();
  for (int i = 1; i <= 5; i++) {
    SCOPED_TRACE(Substitute("restart attempt #$0", i));
    NO_FATALS(ShutdownAndRebuildTablet());
    rpc.Reset();
    req_id.set_attempt_no(req_id.attempt_no() + 1);
    rpc.SetRequestIdPB(unique_ptr<rpc::RequestIdPB>(new rpc::RequestIdPB(req_id)));
    ASSERT_OK(proxy_->Write(req, &resp, &rpc));
    SCOPED_TRACE(resp.DebugString());
    ASSERT_FALSE(resp.has_error());
    ASSERT_EQ(2, resp.per_row_errors_size());
  }
}

// Regression test for KUDU-177. Ensures that after a major delta compaction,
// rows that were in the old DRS's DMS are properly replayed.
TEST_F(TabletServerTest, TestKUDU_177_RecoveryOfDMSEditsAfterMajorDeltaCompaction) {
  // Flush a DRS with 1 rows.
  ANFF(InsertTestRowsRemote(1, 1));
  ASSERT_OK(tablet_replica_->tablet()->Flush());
  ANFF(VerifyRows(schema_, { KeyValue(1, 1) }));

  // Update it, flush deltas.
  ANFF(UpdateTestRowRemote(1, 2));
  ASSERT_OK(tablet_replica_->tablet()->FlushBiggestDMS());

  // Update it again, so this last update is in the DMS.
  ANFF(UpdateTestRowRemote(1, 3));
  ANFF(VerifyRows(schema_, { KeyValue(1, 3) }));

  // Major compact deltas. This doesn't include the DMS, but the old
  // DMS should "move over" to the output of the delta compaction.
  {
    vector<shared_ptr<tablet::RowSet> > rsets;
    tablet_replica_->tablet()->GetRowSetsForTests(&rsets);
    vector<ColumnId> col_ids = { tablet_replica_->tablet()->schema()->column_id(1),
                                 tablet_replica_->tablet()->schema()->column_id(2) };
    ASSERT_OK(tablet_replica_->tablet()->DoMajorDeltaCompaction(col_ids, rsets[0]));
  }
  // Verify that data is still the same.
  ANFF(VerifyRows(schema_, { KeyValue(1, 3) }));

  // Verify that the update remains after a restart.
  ASSERT_OK(ShutdownAndRebuildTablet());
  ANFF(VerifyRows(schema_, { KeyValue(1, 3) }));
}

TEST_F(TabletServerTest, TestClientGetsErrorBackWhenRecoveryFailed) {
  ANFF(InsertTestRowsRemote(1, 7));

  ASSERT_OK(tablet_replica_->tablet()->Flush());

  // Save the log path before shutting down the tablet (and destroying
  // the TabletReplica).
  string log_path = tablet_replica_->log()->ActiveSegmentPathForTests();
  ShutdownTablet();

  ASSERT_OK(log::CorruptLogFile(env_, log_path, log::FLIP_BYTE, 300));

  ASSERT_FALSE(ShutdownAndRebuildTablet().ok());

  // Connect to it.
  CreateTsClientProxies(mini_server_->bound_rpc_addr(),
                        client_messenger_,
                        &tablet_copy_proxy_, &proxy_, &admin_proxy_, &consensus_proxy_,
                        &generic_proxy_);

  WriteRequestPB req;
  req.set_tablet_id(kTabletId);

  WriteResponsePB resp;
  rpc::RpcController controller;

  // We're expecting the write to fail.
  ASSERT_OK(DCHECK_NOTNULL(proxy_.get())->Write(req, &resp, &controller));
  ASSERT_EQ(TabletServerErrorPB::TABLET_FAILED, resp.error().code());
  ASSERT_STR_CONTAINS(resp.error().status().message(), "Tablet not RUNNING: FAILED");

  // Check that the TabletReplica's status message is updated with the failure.
  ASSERT_STR_CONTAINS(tablet_replica_->last_status(),
                      "Log file corruption detected");
}

TEST_F(TabletServerTest, TestReadLatest) {
  int num_rows = AllowSlowTests() ? 10000 : 1000;
  InsertTestRowsDirect(0, num_rows);

  // Instantiate scanner metrics.
  ASSERT_TRUE(mini_server_->server()->metric_entity());
  // We don't care what the function is, since the metric is already instantiated.
  auto active_scanners = METRIC_active_scanners.InstantiateFunctionGauge(
      mini_server_->server()->metric_entity(), Callback<size_t(void)>());
  scoped_refptr<TabletReplica> tablet;
  ASSERT_TRUE(mini_server_->server()->tablet_manager()->LookupTablet(kTabletId, &tablet));
  ASSERT_TRUE(tablet->tablet()->GetMetricEntity());
  scoped_refptr<AtomicGauge<size_t>> tablet_active_scanners =
      METRIC_tablet_active_scanners.Instantiate(tablet->tablet()->GetMetricEntity(), 0);

  ScanResponsePB resp;
  ASSERT_NO_FATAL_FAILURE(OpenScannerWithAllColumns(&resp));

  // Ensure that the scanner ID came back and got inserted into the
  // ScannerManager map.
  string scanner_id = resp.scanner_id();
  ASSERT_TRUE(!scanner_id.empty());
  {
    SharedScanner junk;
    ASSERT_TRUE(mini_server_->server()->scanner_manager()->LookupScanner(scanner_id, &junk));
  }

  // Ensure that the scanner shows up in the server and tablet's metrics.
  ASSERT_EQ(1, active_scanners->value());
  ASSERT_EQ(1, tablet_active_scanners->value());

  // Drain all the rows from the scanner.
  vector<string> results;
  ASSERT_NO_FATAL_FAILURE(DrainScannerToStrings(resp.scanner_id(), schema_, &results));
  ASSERT_EQ(num_rows, results.size());

  KuduPartialRow row(&schema_);
  for (int i = 0; i < num_rows; i++) {
    BuildTestRow(i, &row);
    string expected = "(" + row.ToString() + ")";
    ASSERT_EQ(expected, results[i]);
  }

  // Since the rows are drained, the scanner should be automatically removed
  // from the scanner manager.
  {
    SharedScanner junk;
    ASSERT_FALSE(mini_server_->server()->scanner_manager()->LookupScanner(scanner_id, &junk));
  }

  // Ensure that the metrics have been updated now that the scanner is unregistered.
  ASSERT_EQ(0, active_scanners->value());
  ASSERT_EQ(0, tablet_active_scanners->value());
}

class ExpiredScannerParamTest :
    public TabletServerTest,
    public ::testing::WithParamInterface<ReadMode> {
};

TEST_P(ExpiredScannerParamTest, Test) {
  const ReadMode mode = GetParam();

  // Make scanners expire quickly.
  FLAGS_scanner_ttl_ms = 1;

  int num_rows = 100;
  InsertTestRowsDirect(0, num_rows);

  // Instantiate scanners expired metric.
  ASSERT_TRUE(mini_server_->server()->metric_entity());
  scoped_refptr<Counter> scanners_expired = METRIC_scanners_expired.Instantiate(
      mini_server_->server()->metric_entity());

  // Initially, there've been no scanners, so none of have expired.
  ASSERT_EQ(0, scanners_expired->value());

  // Open a scanner but don't read from it.
  ScanResponsePB resp;
  ASSERT_NO_FATAL_FAILURE(OpenScannerWithAllColumns(&resp, mode));

  // The scanner should expire after a short time.
  ASSERT_EVENTUALLY([&]() {
    ASSERT_EQ(1, scanners_expired->value());
  });

  // Continue the scan. We should get a SCANNER_EXPIRED error.
  ScanRequestPB req;
  RpcController rpc;
  req.set_scanner_id(resp.scanner_id());
  req.set_call_seq_id(1);
  resp.Clear();
  ASSERT_OK(proxy_->Scan(req, &resp, &rpc));
  ASSERT_TRUE(resp.has_error());
  ASSERT_EQ(TabletServerErrorPB::SCANNER_EXPIRED, resp.error().code());
}

const ReadMode read_modes[] = {
    READ_LATEST,
    READ_AT_SNAPSHOT,
    READ_YOUR_WRITES,
};

INSTANTIATE_TEST_CASE_P(Params, ExpiredScannerParamTest,
                        testing::ValuesIn(read_modes));

class ScanCorruptedDeltasParamTest :
    public TabletServerTest,
    public ::testing::WithParamInterface<ReadMode> {
};

TEST_P(ScanCorruptedDeltasParamTest, Test) {
  const ReadMode mode = GetParam();
  // Ensure some rows get to disk with deltas.
  InsertTestRowsDirect(0, 100);
  ASSERT_OK(tablet_replica_->tablet()->Flush());
  UpdateTestRowRemote(1, 100);
  ASSERT_OK(tablet_replica_->tablet()->Flush());

  // Fudge with some delta blocks.
  TabletSuperBlockPB superblock_pb;
  tablet_replica_->tablet()->metadata()->ToSuperBlock(&superblock_pb);
  FsManager* fs_manager = mini_server_->server()->fs_manager();
  for (int rowset_no = 0; rowset_no < superblock_pb.rowsets_size(); rowset_no++) {
    RowSetDataPB* rowset_pb = superblock_pb.mutable_rowsets(rowset_no);
    for (int id = 0; id < rowset_pb->undo_deltas_size(); id++) {
      BlockId block_id(rowset_pb->undo_deltas(id).block().id());
      BlockId new_block_id;
      // Make a copy of each block and rewrite the superblock to include these
      // newly corrupted blocks.
      ASSERT_OK(CreateCorruptBlock(fs_manager, block_id, 0, 0, &new_block_id));
      rowset_pb->mutable_undo_deltas(id)->mutable_block()->set_id(new_block_id.id());
    }
  }
  // Grab the deltafiles and corrupt them.
  const string& meta_path = fs_manager->GetTabletMetadataPath(tablet_replica_->tablet_id());
  ShutdownTablet();

  // Flush the corruption and rebuild the server with the corrupt data.
  ASSERT_OK(pb_util::WritePBContainerToPath(env_,
      meta_path, superblock_pb, pb_util::OVERWRITE, pb_util::SYNC));
  ASSERT_OK(ShutdownAndRebuildTablet());
  LOG(INFO) << Substitute("Rebuilt tablet $0 with broken blocks", tablet_replica_->tablet_id());

  // Now open a scanner for the server.
  ScanRequestPB req;
  ScanResponsePB resp;
  RpcController rpc;
  NewScanRequestPB* scan = req.mutable_new_scan_request();
  scan->set_tablet_id(kTabletId);
  scan->set_read_mode(mode);
  ASSERT_OK(SchemaToColumnPBs(schema_, scan->mutable_projected_columns()));

  // Send the call. This first call should attempt to init the corrupted
  // deltafiles and return with an error. The second call should see that the
  // previous call to init failed and should return the failed status.
  req.set_batch_size_bytes(10000);
  for (int i = 0; i < 2; i++) {
    rpc.Reset();
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(proxy_->Scan(req, &resp, &rpc));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_TRUE(resp.has_error());
    ASSERT_EQ(resp.error().status().code(), AppStatusPB::CORRUPTION);
    ASSERT_STR_CONTAINS(resp.error().status().message(), "failed to init CFileReader");
  }
}

INSTANTIATE_TEST_CASE_P(Params, ScanCorruptedDeltasParamTest,
                        testing::ValuesIn(read_modes));

class ScannerOpenWhenServerShutsDownParamTest :
    public TabletServerTest,
    public ::testing::WithParamInterface<ReadMode> {
};
TEST_P(ScannerOpenWhenServerShutsDownParamTest, Test) {
  const ReadMode mode = GetParam();
  // Write and flush the write, so we have some rows in MRS and DRS
  InsertTestRowsDirect(0, 100);
  ASSERT_OK(tablet_replica_->tablet()->Flush());
  UpdateTestRowRemote(1, 100);
  ASSERT_OK(tablet_replica_->tablet()->Flush());

  ScanResponsePB resp;
  ASSERT_NO_FATAL_FAILURE(OpenScannerWithAllColumns(&resp, mode));

  // Scanner is now open. The test will now shut down the TS with the scanner still
  // out there. Due to KUDU-161 this used to fail, since the scanner (and thus the MRS)
  // stayed open longer than the anchor registry
}

INSTANTIATE_TEST_CASE_P(Params, ScannerOpenWhenServerShutsDownParamTest,
                        testing::ValuesIn(read_modes));

TEST_F(TabletServerTest, TestSnapshotScan) {
  const int num_rows = AllowSlowTests() ? 1000 : 100;
  const int num_batches = AllowSlowTests() ? 100 : 10;
  vector<uint64_t> write_timestamps_collector;

  // perform a series of writes and collect the timestamps
  InsertTestRowsRemote(0, num_rows, num_batches, nullptr,
                       kTabletId, &write_timestamps_collector);

  // now perform snapshot scans.
  ScanRequestPB req;
  ScanResponsePB resp;
  RpcController rpc;

  int batch_idx = 1;
  for (uint64_t write_timestamp : write_timestamps_collector) {
    req.Clear();
    resp.Clear();
    rpc.Reset();
    // Set up a new request with no predicates, all columns.
    const Schema& projection = schema_;
    NewScanRequestPB* scan = req.mutable_new_scan_request();
    scan->set_tablet_id(kTabletId);
    scan->set_read_mode(READ_AT_SNAPSHOT);

    // Decode and re-encode the timestamp. Note that a snapshot at 'write_timestamp'
    // does not include the written rows, so we increment that timestamp by one
    // to make sure we get those rows back
    Timestamp read_timestamp(write_timestamp);
    read_timestamp = Timestamp(read_timestamp.value() + 1);
    scan->set_snap_timestamp(read_timestamp.ToUint64());

    ASSERT_OK(SchemaToColumnPBs(projection, scan->mutable_projected_columns()));
    req.set_call_seq_id(0);

    const Timestamp pre_scan_ts = mini_server_->server()->clock()->Now();
    // Send the call
    {
      SCOPED_TRACE(SecureDebugString(req));
      req.set_batch_size_bytes(0); // so it won't return data right away
      ASSERT_OK(proxy_->Scan(req, &resp, &rpc));
      SCOPED_TRACE(SecureDebugString(resp));
      ASSERT_FALSE(resp.has_error());
    }

    // The 'propagated_timestamp' field must be set for 'success' responses.
    ASSERT_TRUE(resp.has_propagated_timestamp());
    ASSERT_GT(mini_server_->server()->clock()->Now().ToUint64(),
              resp.propagated_timestamp());
    ASSERT_LT(pre_scan_ts.ToUint64(), resp.propagated_timestamp());

    ASSERT_TRUE(resp.has_more_results());
    // Drain all the rows from the scanner.
    vector<string> results;
    ASSERT_NO_FATAL_FAILURE(DrainScannerToStrings(resp.scanner_id(), schema_, &results));
    // on each scan we should get (num_rows / num_batches) * batch_idx rows back
    int expected_num_rows = (num_rows / num_batches) * batch_idx;
    ASSERT_EQ(expected_num_rows, results.size());

    if (VLOG_IS_ON(2)) {
      VLOG(2) << "Scanner: " << resp.scanner_id() << " performing a snapshot read at: "
              << read_timestamp.ToString() << " got back: ";
      for (const string& result : results) {
        VLOG(2) << result;
      }
    }

    // assert that the first and last rows were the expected ones
    ASSERT_EQ(R"((int32 key=0, int32 int_val=0, string string_val="original0"))", results[0]);
    ASSERT_EQ(Substitute(R"((int32 key=$0, int32 int_val=$0, string string_val="original$0"))",
                         (batch_idx * (num_rows / num_batches) - 1)), results[results.size() - 1]);
    batch_idx++;
  }
}

TEST_F(TabletServerTest, TestSnapshotScan_WithoutSnapshotTimestamp) {
  vector<uint64_t> write_timestamps_collector;
  // perform a write
  InsertTestRowsRemote(0, 1, 1, nullptr, kTabletId, &write_timestamps_collector);

  ScanRequestPB req;
  ScanResponsePB resp;
  RpcController rpc;

  // Set up a new request with no predicates, all columns.
  const Schema& projection = schema_;
  NewScanRequestPB* scan = req.mutable_new_scan_request();
  scan->set_tablet_id(kTabletId);
  ASSERT_OK(SchemaToColumnPBs(projection, scan->mutable_projected_columns()));
  req.set_call_seq_id(0);
  req.set_batch_size_bytes(0); // so it won't return data right away
  scan->set_read_mode(READ_AT_SNAPSHOT);

  const Timestamp pre_scan_ts = mini_server_->server()->clock()->Now();

  // Send the call
  {
    SCOPED_TRACE(SecureDebugString(req));
    req.set_batch_size_bytes(0); // so it won't return data right away
    ASSERT_OK(proxy_->Scan(req, &resp, &rpc));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_FALSE(resp.has_error());
  }

  // make sure that the snapshot timestamp that was selected is >= now
  ASSERT_GE(resp.snap_timestamp(), pre_scan_ts.ToUint64());
  // The 'propagated_timestamp' field must be set for all successful responses.
  ASSERT_TRUE(resp.has_propagated_timestamp());
  ASSERT_GT(mini_server_->server()->clock()->Now().ToUint64(),
            resp.propagated_timestamp());
  ASSERT_LT(pre_scan_ts.ToUint64(), resp.propagated_timestamp());
  // The propagated timestamp should be after (i.e. greater) than the scan
  // timestamp.
  ASSERT_GT(resp.propagated_timestamp(), resp.snap_timestamp());
}

// Tests that a snapshot in the future (beyond the current time plus maximum
// synchronization error) fails as an invalid snapshot.
TEST_F(TabletServerTest, TestSnapshotScan_SnapshotInTheFutureFails) {
  vector<uint64_t> write_timestamps_collector;
  // perform a write
  InsertTestRowsRemote(0, 1, 1, nullptr, kTabletId, &write_timestamps_collector);

  ScanRequestPB req;
  ScanResponsePB resp;
  RpcController rpc;

  // Set up a new request with no predicates, all columns.
  const Schema& projection = schema_;
  NewScanRequestPB* scan = req.mutable_new_scan_request();
  scan->set_tablet_id(kTabletId);
  ASSERT_OK(SchemaToColumnPBs(projection, scan->mutable_projected_columns()));
  req.set_call_seq_id(0);
  req.set_batch_size_bytes(0); // so it won't return data right away
  scan->set_read_mode(READ_AT_SNAPSHOT);

  Timestamp read_timestamp(write_timestamps_collector[0]);
  // Increment the write timestamp by 60 secs: the server will definitely consider
  // this in the future.
  read_timestamp = HybridClock::TimestampFromMicroseconds(
      HybridClock::GetPhysicalValueMicros(read_timestamp) + 60000000);
  scan->set_snap_timestamp(read_timestamp.ToUint64());

  // Send the call
  {
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(proxy_->Scan(req, &resp, &rpc));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_TRUE(resp.has_error());
    ASSERT_EQ(TabletServerErrorPB::INVALID_SNAPSHOT, resp.error().code());
  }
}

// Test retrying a snapshot scan using last_row.
TEST_F(TabletServerTest, TestSnapshotScan_LastRow) {
  // Set the internal batching within the tserver to be small. Otherwise,
  // even though we use a small batch size in our request, we'd end up reading
  // many rows at a time.
  FLAGS_scanner_batch_size_rows = 5;
  const int num_rows = AllowSlowTests() ? 1000 : 100;
  const int num_batches = AllowSlowTests() ? 10 : 5;
  const int batch_size = num_rows / num_batches;

  // Generate some interleaved rows
  for (int i = 0; i < batch_size; i++) {
    ASSERT_OK(tablet_replica_->tablet()->Flush());
    for (int j = 0; j < num_rows; j++) {
      if (j % batch_size == i) {
        InsertTestRowsDirect(j, 1);
      }
    }
  }

  // Remove all the key columns from the projection.
  // This makes sure the scanner adds them in for sorting but removes them before returning
  // to the client.
  SchemaBuilder sb(schema_);
  for (int i = 0; i < schema_.num_key_columns(); i++) {
    sb.RemoveColumn(schema_.column(i).name());
  }
  const Schema& projection = sb.BuildWithoutIds();

  // Scan the whole tablet with a few different batch sizes.
  for (int i = 1; i < 10000; i *= 2) {
    ScanResponsePB resp;
    ScanRequestPB req;
    RpcController rpc;

    // Set up a new snapshot scan without a specified timestamp.
    NewScanRequestPB* scan = req.mutable_new_scan_request();
    scan->set_tablet_id(kTabletId);
    ASSERT_OK(SchemaToColumnPBs(projection, scan->mutable_projected_columns()));
    req.set_call_seq_id(0);
    scan->set_read_mode(READ_AT_SNAPSHOT);
    scan->set_order_mode(ORDERED);

    // Send the call
    {
      SCOPED_TRACE(SecureDebugString(req));
      req.set_batch_size_bytes(0); // so it won't return data right away
      ASSERT_OK(proxy_->Scan(req, &resp, &rpc));
      SCOPED_TRACE(SecureDebugString(resp));
      ASSERT_FALSE(resp.has_error());
    }

    vector<string> results;
    do {
      rpc.Reset();
      // Send the call.
      {
        SCOPED_TRACE(SecureDebugString(req));
        req.set_batch_size_bytes(i);
        ASSERT_OK(proxy_->Scan(req, &resp, &rpc));
        SCOPED_TRACE(SecureDebugString(resp));
        ASSERT_FALSE(resp.has_error());
      }
      // Save the rows into 'results' vector.
      StringifyRowsFromResponse(projection, rpc, &resp, &results);
      // Retry the scan, setting the last_row_key and snapshot based on the response.
      scan->set_last_primary_key(resp.last_primary_key());
      scan->set_snap_timestamp(resp.snap_timestamp());
    } while (resp.has_more_results());

    ASSERT_EQ(num_rows, results.size());

    // Verify that we get the rows back in order.
    KuduPartialRow row(&projection);
    for (int j = 0; j < num_rows; j++) {
      ASSERT_OK(row.SetInt32(0, j * 2));
      ASSERT_OK(row.SetStringCopy(1, StringPrintf("hello %d", j)));
      string expected = "(" + row.ToString() + ")";
      ASSERT_EQ(expected, results[j]);
    }
  }
}

// Tests that a read in the future succeeds if a propagated_timestamp (that is even
// further in the future) follows along. Also tests that the clock was updated so
// that no writes will ever have a timestamp post this snapshot.
TEST_F(TabletServerTest, TestSnapshotScan_SnapshotInTheFutureWithPropagatedTimestamp) {
  vector<uint64_t> write_timestamps_collector;
  // perform a write
  InsertTestRowsRemote(0, 1, 1, nullptr, kTabletId, &write_timestamps_collector);

  ScanRequestPB req;
  ScanResponsePB resp;
  RpcController rpc;

  // Set up a new request with no predicates, all columns.
  const Schema& projection = schema_;
  NewScanRequestPB* scan = req.mutable_new_scan_request();
  scan->set_tablet_id(kTabletId);
  ASSERT_OK(SchemaToColumnPBs(projection, scan->mutable_projected_columns()));
  req.set_call_seq_id(0);
  req.set_batch_size_bytes(0); // so it won't return data right away
  scan->set_read_mode(READ_AT_SNAPSHOT);

  Timestamp read_timestamp(write_timestamps_collector[0]);
  // increment the write timestamp by 5 secs, the server will definitely consider
  // this in the future.
  read_timestamp = HybridClock::TimestampFromMicroseconds(
      HybridClock::GetPhysicalValueMicros(read_timestamp) + 5000000);
  scan->set_snap_timestamp(read_timestamp.ToUint64());

  // send a propagated timestamp that is an additional 100 msecs into the future.
  Timestamp propagated_timestamp = HybridClock::TimestampFromMicroseconds(
      HybridClock::GetPhysicalValueMicros(read_timestamp) + 100000);
  scan->set_propagated_timestamp(propagated_timestamp.ToUint64());

  // Send the call
  {
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(proxy_->Scan(req, &resp, &rpc));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_FALSE(resp.has_error());
  }

  // make sure the server's current clock returns a value that is larger than the
  // propagated timestamp. It should have the same physical time, but higher
  // logical time (due to various calls to clock.Now() when processing the request).
  Timestamp now = mini_server_->server()->clock()->Now();

  ASSERT_EQ(HybridClock::GetPhysicalValueMicros(propagated_timestamp),
            HybridClock::GetPhysicalValueMicros(now));

  ASSERT_GT(HybridClock::GetLogicalValue(now),
            HybridClock::GetLogicalValue(propagated_timestamp));

  vector<string> results;
  ASSERT_NO_FATAL_FAILURE(DrainScannerToStrings(resp.scanner_id(), schema_, &results));
  ASSERT_EQ(1, results.size());
  ASSERT_EQ(R"((int32 key=0, int32 int_val=0, string string_val="original0"))", results[0]);
}


// Test that a read in the future fails, even if a propagated_timestamp is sent along,
// if the read_timestamp is beyond the propagated_timestamp.
TEST_F(TabletServerTest, TestSnapshotScan__SnapshotInTheFutureBeyondPropagatedTimestampFails) {
  vector<uint64_t> write_timestamps_collector;
  // perform a write
  InsertTestRowsRemote(0, 1, 1, nullptr, kTabletId, &write_timestamps_collector);

  ScanRequestPB req;
  ScanResponsePB resp;
  RpcController rpc;

  // Set up a new request with no predicates, all columns.
  const Schema& projection = schema_;
  NewScanRequestPB* scan = req.mutable_new_scan_request();
  scan->set_tablet_id(kTabletId);
  ASSERT_OK(SchemaToColumnPBs(projection, scan->mutable_projected_columns()));
  req.set_call_seq_id(0);
  req.set_batch_size_bytes(0); // so it won't return data right away
  scan->set_read_mode(READ_AT_SNAPSHOT);

  Timestamp read_timestamp(write_timestamps_collector[0]);
  // increment the write timestamp by 60 secs, the server will definitely consider
  // this in the future.
  read_timestamp = HybridClock::TimestampFromMicroseconds(
      HybridClock::GetPhysicalValueMicros(read_timestamp) + 60000000);
  scan->set_snap_timestamp(read_timestamp.ToUint64());

  // send a propagated timestamp that is an less than the read timestamp (but still
  // in the future as far the server is concerned).
  Timestamp propagated_timestamp = HybridClock::TimestampFromMicroseconds(
      HybridClock::GetPhysicalValueMicros(read_timestamp) - 100000);
  scan->set_propagated_timestamp(propagated_timestamp.ToUint64());

  // Send the call
  {
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(proxy_->Scan(req, &resp, &rpc));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_TRUE(resp.has_error());
    ASSERT_EQ(TabletServerErrorPB::INVALID_SNAPSHOT, resp.error().code());
  }
}

// Scan with READ_YOUR_WRITES mode to ensure it can
// satisfy read-your-writes/read-your-reads session guarantee.
TEST_F(TabletServerTest, TestScanYourWrites) {
  vector<uint64_t> write_timestamps_collector;
  const int kNumRows = 100;
  // Perform a write.
  InsertTestRowsRemote(0, kNumRows, 1, nullptr, kTabletId, &write_timestamps_collector);

  // Scan with READ_YOUR_WRITES mode and use the previous
  // write response as the propagated timestamp.
  ScanResponsePB resp;
  int64_t propagated_timestamp = write_timestamps_collector[0];
  ScanYourWritesTest(propagated_timestamp, &resp);

  // Store the returned snapshot timestamp as the propagated
  // timestamp for the next read.
  propagated_timestamp = resp.snap_timestamp();
  // Drain all the rows from the scanner.
  vector<string> results;
  ASSERT_NO_FATAL_FAILURE(DrainScannerToStrings(resp.scanner_id(), schema_, &results));
  ASSERT_EQ(kNumRows, results.size());
  ASSERT_EQ(R"((int32 key=0, int32 int_val=0, string string_val="original0"))", results[0]);
  ASSERT_EQ(R"((int32 key=99, int32 int_val=99, string string_val="original99"))", results[99]);

  // Rescan the tablet to ensure READ_YOUR_WRITES mode can
  // satisfy read-your-reads session guarantee.
  ScanResponsePB new_resp;
  ScanYourWritesTest(propagated_timestamp, &new_resp);
  // Drain all the rows from the scanner.
  results.clear();
  ASSERT_NO_FATAL_FAILURE(DrainScannerToStrings(new_resp.scanner_id(), schema_, &results));
  ASSERT_EQ(kNumRows, results.size());
  ASSERT_EQ(R"((int32 key=0, int32 int_val=0, string string_val="original0"))", results[0]);
  ASSERT_EQ(R"((int32 key=99, int32 int_val=99, string string_val="original99"))", results[99]);
}

// Tests that a read succeeds even without propagated_timestamp.
TEST_F(TabletServerTest, TestScanYourWrites_WithoutPropagatedTimestamp) {
  vector<uint64_t> write_timestamps_collector;
  // Perform a write.
  InsertTestRowsRemote(0, 1, 1, nullptr, kTabletId, &write_timestamps_collector);

  ScanResponsePB resp;
  ScanYourWritesTest(Timestamp::kMin.ToUint64(), &resp);
}

// Tests that a read succeeds even with a future propagated_timestamp. Also
// tests that the clock was updated so that no writes will ever have a
// timestamp before this snapshot.
TEST_F(TabletServerTest, TestScanYourWrites_PropagatedTimestampInTheFuture) {
  vector<uint64_t> write_timestamps_collector;
  // Perform a write.
  InsertTestRowsRemote(0, 1, 1, nullptr, kTabletId, &write_timestamps_collector);

  ScanResponsePB resp;
  // Increment the write timestamp by 5 secs: the server will definitely consider
  // this in the future.
  Timestamp propagated_timestamp(write_timestamps_collector[0]);
  propagated_timestamp = HybridClock::TimestampFromMicroseconds(
      HybridClock::GetPhysicalValueMicros(propagated_timestamp) + 5000000);
  ScanYourWritesTest(propagated_timestamp.ToUint64(), &resp);

  // Make sure the server's current clock returns a value that is larger than the
  // propagated timestamp. It should have the same physical time, but higher
  // logical time (due to various calls to clock.Now() when processing the request).
  Timestamp now = mini_server_->server()->clock()->Now();

  ASSERT_EQ(HybridClock::GetPhysicalValueMicros(propagated_timestamp),
            HybridClock::GetPhysicalValueMicros(now));

  ASSERT_GT(HybridClock::GetLogicalValue(now),
            HybridClock::GetLogicalValue(propagated_timestamp));

  vector<string> results;
  NO_FATALS(DrainScannerToStrings(resp.scanner_id(), schema_, &results));
  ASSERT_EQ(1, results.size());
  ASSERT_EQ(R"((int32 key=0, int32 int_val=0, string string_val="original0"))", results[0]);
}

TEST_F(TabletServerTest, TestScanWithStringPredicates) {
  InsertTestRowsDirect(0, 100);

  ScanRequestPB req;
  ScanResponsePB resp;
  RpcController rpc;

  NewScanRequestPB* scan = req.mutable_new_scan_request();
  scan->set_tablet_id(kTabletId);
  req.set_batch_size_bytes(0); // so it won't return data right away
  ASSERT_OK(SchemaToColumnPBs(schema_, scan->mutable_projected_columns()));

  // Set up a range predicate: "hello 50" < string_val <= "hello 59"
  ColumnRangePredicatePB* pred = scan->add_deprecated_range_predicates();
  pred->mutable_column()->CopyFrom(scan->projected_columns(2));

  pred->set_lower_bound("hello 50");
  pred->set_inclusive_upper_bound("hello 59");

  // Send the call
  {
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(proxy_->Scan(req, &resp, &rpc));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_FALSE(resp.has_error());
  }

  // Drain all the rows from the scanner.
  vector<string> results;
  ASSERT_NO_FATAL_FAILURE(
    DrainScannerToStrings(resp.scanner_id(), schema_, &results));
  ASSERT_EQ(10, results.size());
  ASSERT_EQ(R"((int32 key=50, int32 int_val=100, string string_val="hello 50"))", results[0]);
  ASSERT_EQ(R"((int32 key=59, int32 int_val=118, string string_val="hello 59"))", results[9]);
}

TEST_F(TabletServerTest, TestScanWithPredicates) {
  // TODO: need to test adding a predicate on a column which isn't part of the
  // projection! I don't think we implemented this at the tablet layer yet,
  // but should do so.

  int num_rows = AllowSlowTests() ? 10000 : 1000;
  InsertTestRowsDirect(0, num_rows);

  ScanRequestPB req;
  ScanResponsePB resp;
  RpcController rpc;

  NewScanRequestPB* scan = req.mutable_new_scan_request();
  scan->set_tablet_id(kTabletId);
  req.set_batch_size_bytes(0); // so it won't return data right away
  ASSERT_OK(SchemaToColumnPBs(schema_, scan->mutable_projected_columns()));

  // Set up a range predicate: 51 <= key <= 100
  ColumnRangePredicatePB* pred = scan->add_deprecated_range_predicates();
  pred->mutable_column()->CopyFrom(scan->projected_columns(0));

  int32_t lower_bound_int = 51;
  int32_t upper_bound_int = 100;
  pred->mutable_lower_bound()->append(reinterpret_cast<char*>(&lower_bound_int),
                                      sizeof(lower_bound_int));
  pred->mutable_inclusive_upper_bound()->append(reinterpret_cast<char*>(&upper_bound_int),
                                                sizeof(upper_bound_int));

  // Send the call
  {
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(proxy_->Scan(req, &resp, &rpc));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_FALSE(resp.has_error());
  }

  // Drain all the rows from the scanner.
  vector<string> results;
  ASSERT_NO_FATAL_FAILURE(
    DrainScannerToStrings(resp.scanner_id(), schema_, &results));
  ASSERT_EQ(50, results.size());
}

TEST_F(TabletServerTest, TestScanWithEncodedPredicates) {
  InsertTestRowsDirect(0, 100);

  ScanRequestPB req;
  ScanResponsePB resp;
  RpcController rpc;

  NewScanRequestPB* scan = req.mutable_new_scan_request();
  scan->set_tablet_id(kTabletId);
  req.set_batch_size_bytes(0); // so it won't return data right away
  ASSERT_OK(SchemaToColumnPBs(schema_, scan->mutable_projected_columns()));

  // Set up a range predicate: 51 <= key <= 60
  // using encoded keys
  int32_t start_key_int = 51;
  int32_t stop_key_int = 60;
  EncodedKeyBuilder ekb(&schema_);
  ekb.AddColumnKey(&start_key_int);
  gscoped_ptr<EncodedKey> start_encoded(ekb.BuildEncodedKey());

  ekb.Reset();
  ekb.AddColumnKey(&stop_key_int);
  gscoped_ptr<EncodedKey> stop_encoded(ekb.BuildEncodedKey());

  scan->mutable_start_primary_key()->assign(
    reinterpret_cast<const char*>(start_encoded->encoded_key().data()),
    start_encoded->encoded_key().size());
  scan->mutable_stop_primary_key()->assign(
    reinterpret_cast<const char*>(stop_encoded->encoded_key().data()),
    stop_encoded->encoded_key().size());

  // Send the call
  {
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(proxy_->Scan(req, &resp, &rpc));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_FALSE(resp.has_error());
  }

  // Drain all the rows from the scanner.
  vector<string> results;
  ASSERT_NO_FATAL_FAILURE(
    DrainScannerToStrings(resp.scanner_id(), schema_, &results));
  ASSERT_EQ(9, results.size());
  EXPECT_EQ(R"((int32 key=51, int32 int_val=102, string string_val="hello 51"))",
            results.front());
  EXPECT_EQ(R"((int32 key=59, int32 int_val=118, string string_val="hello 59"))",
            results.back());
}


// Test requesting more rows from a scanner which doesn't exist
TEST_F(TabletServerTest, TestBadScannerID) {
  ScanRequestPB req;
  ScanResponsePB resp;
  RpcController rpc;

  req.set_scanner_id("does-not-exist");

  SCOPED_TRACE(SecureDebugString(req));
  ASSERT_OK(proxy_->Scan(req, &resp, &rpc));
  SCOPED_TRACE(SecureDebugString(resp));
  ASSERT_TRUE(resp.has_error());
  ASSERT_EQ(TabletServerErrorPB::SCANNER_EXPIRED, resp.error().code());
}

// Test passing a scanner ID, but also filling in some of the NewScanRequest
// field.
class InvalidScanRequest_NewScanAndScannerIDParamTest :
    public TabletServerTest,
    public ::testing::WithParamInterface<ReadMode> {
};
TEST_P(InvalidScanRequest_NewScanAndScannerIDParamTest, Test) {
  const ReadMode mode = GetParam();
  ScanRequestPB req;
  ScanResponsePB resp;
  RpcController rpc;

  NewScanRequestPB* scan = req.mutable_new_scan_request();
  scan->set_tablet_id(kTabletId);
  scan->set_read_mode(mode);
  req.set_batch_size_bytes(0); // so it won't return data right away
  req.set_scanner_id("x");
  SCOPED_TRACE(SecureDebugString(req));
  Status s = proxy_->Scan(req, &resp, &rpc);
  ASSERT_FALSE(s.ok());
  ASSERT_STR_CONTAINS(s.ToString(), "Must not pass both a scanner_id and new_scan_request");
}

INSTANTIATE_TEST_CASE_P(Params, InvalidScanRequest_NewScanAndScannerIDParamTest,
                        testing::ValuesIn(read_modes));

// Test that passing a projection with fields not present in the tablet schema
// throws an exception.
TEST_F(TabletServerTest, TestInvalidScanRequest_BadProjection) {
  const Schema projection({ ColumnSchema("col_doesnt_exist", INT32) }, 0);
  VerifyScanRequestFailure(projection,
                           TabletServerErrorPB::MISMATCHED_SCHEMA,
                           "Some columns are not present in the current schema: col_doesnt_exist");
}

// Test that passing a projection with mismatched type/nullability throws an exception.
TEST_F(TabletServerTest, TestInvalidScanRequest_BadProjectionTypes) {
  Schema projection;

  // Verify mismatched nullability for the not-null int field
  ASSERT_OK(
    projection.Reset({ ColumnSchema("int_val", INT32, true) }, // should be NOT NULL
                     0));
  VerifyScanRequestFailure(projection,
                           TabletServerErrorPB::MISMATCHED_SCHEMA,
                           "The column 'int_val' must have type int32 NOT "
                           "NULL found int32 NULLABLE");

  // Verify mismatched nullability for the nullable string field
  ASSERT_OK(
    projection.Reset({ ColumnSchema("string_val", STRING, false) }, // should be NULLABLE
                     0));
  VerifyScanRequestFailure(projection,
                           TabletServerErrorPB::MISMATCHED_SCHEMA,
                           "The column 'string_val' must have type string "
                           "NULLABLE found string NOT NULL");

  // Verify mismatched type for the not-null int field
  ASSERT_OK(
    projection.Reset({ ColumnSchema("int_val", INT16, false) },     // should be INT32 NOT NULL
                     0));
  VerifyScanRequestFailure(projection,
                           TabletServerErrorPB::MISMATCHED_SCHEMA,
                           "The column 'int_val' must have type int32 NOT "
                           "NULL found int16 NOT NULL");

  // Verify mismatched type for the nullable string field
  ASSERT_OK(projection.Reset(
        { ColumnSchema("string_val", INT32, true) }, // should be STRING NULLABLE
        0));
  VerifyScanRequestFailure(projection,
                           TabletServerErrorPB::MISMATCHED_SCHEMA,
                           "The column 'string_val' must have type string "
                           "NULLABLE found int32 NULLABLE");
}

// Test that passing a projection with Column IDs throws an exception.
// Column IDs are assigned to the user request schema on the tablet server
// based on the latest schema.
class InvalidScanRequest_WithIdsParamTest :
    public TabletServerTest,
    public ::testing::WithParamInterface<ReadMode> {
};
TEST_P(InvalidScanRequest_WithIdsParamTest, Test) {
  const Schema* projection = tablet_replica_->tablet()->schema();
  ASSERT_TRUE(projection->has_column_ids());
  VerifyScanRequestFailure(*projection,
                           TabletServerErrorPB::INVALID_SCHEMA,
                           "User requests should not have Column IDs");
}

INSTANTIATE_TEST_CASE_P(Params, InvalidScanRequest_WithIdsParamTest,
                        testing::ValuesIn(read_modes));

// Test scanning a tablet that has no entries.
TEST_F(TabletServerTest, TestScan_NoResults) {
  ScanRequestPB req;
  ScanResponsePB resp;
  RpcController rpc;

  // Set up a new request with no predicates, all columns.
  const Schema& projection = schema_;
  NewScanRequestPB* scan = req.mutable_new_scan_request();
  scan->set_tablet_id(kTabletId);
  req.set_batch_size_bytes(0); // so it won't return data right away
  ASSERT_OK(SchemaToColumnPBs(projection, scan->mutable_projected_columns()));
  req.set_call_seq_id(0);

  // Send the call
  {
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(proxy_->Scan(req, &resp, &rpc));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_FALSE(resp.has_error());

    // Because there are no entries, we should immediately return "no results".
    ASSERT_FALSE(resp.has_more_results());
  }
}

// Test scanning a tablet that has no entries.
class InvalidScanSeqIdParamTest :
    public TabletServerTest,
    public ::testing::WithParamInterface<ReadMode> {
};
TEST_P(InvalidScanSeqIdParamTest, Test) {
  const ReadMode mode = GetParam();
  InsertTestRowsDirect(0, 10);

  ScanRequestPB req;
  ScanResponsePB resp;
  RpcController rpc;

  {
    // Set up a new scan request with no predicates, all columns.
    const Schema& projection = schema_;
    NewScanRequestPB* scan = req.mutable_new_scan_request();
    scan->set_tablet_id(kTabletId);
    scan->set_read_mode(mode);
    ASSERT_OK(SchemaToColumnPBs(projection, scan->mutable_projected_columns()));
    req.set_call_seq_id(0);
    req.set_batch_size_bytes(0); // so it won't return data right away

    // Create the scanner
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(proxy_->Scan(req, &resp, &rpc));
    ASSERT_FALSE(resp.has_error());
    ASSERT_TRUE(resp.has_more_results());
  }

  string scanner_id = resp.scanner_id();
  resp.Clear();

  {
    // Continue the scan with an invalid sequence ID
    req.Clear();
    rpc.Reset();
    req.set_scanner_id(scanner_id);
    req.set_batch_size_bytes(0); // so it won't return data right away
    req.set_call_seq_id(42); // should be 1

    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(proxy_->Scan(req, &resp, &rpc));
    ASSERT_TRUE(resp.has_error());
    ASSERT_EQ(TabletServerErrorPB::INVALID_SCAN_CALL_SEQ_ID, resp.error().code());
  }
}

INSTANTIATE_TEST_CASE_P(Params, InvalidScanSeqIdParamTest,
                        testing::ValuesIn(read_modes));

// Regression test for KUDU-1789: when ScannerKeepAlive is called on a non-existent
// scanner, it should properly respond with an error.
TEST_F(TabletServerTest, TestScan_KeepAliveExpiredScanner) {
  ScannerKeepAliveRequestPB req;
  ScannerKeepAliveResponsePB resp;
  RpcController rpc;

  rpc.set_timeout(MonoDelta::FromSeconds(5));
  req.set_scanner_id("does-not-exist");
  ASSERT_OK(proxy_->ScannerKeepAlive(req, &resp, &rpc));
  ASSERT_TRUE(resp.has_error());
  ASSERT_EQ(resp.error().code(), TabletServerErrorPB::SCANNER_EXPIRED);
}

void TabletServerTest::ScanYourWritesTest(uint64_t propagated_timestamp,
                                          ScanResponsePB* resp) {
  ScanRequestPB req;

  // Set up a new request with no predicates, all columns.
  const Schema &projection = schema_;
  NewScanRequestPB *scan = req.mutable_new_scan_request();
  scan->set_tablet_id(kTabletId);
  scan->set_read_mode(READ_YOUR_WRITES);
  if (propagated_timestamp != Timestamp::kInvalidTimestamp.ToUint64()) {
    scan->set_propagated_timestamp(propagated_timestamp);
  }
  ASSERT_OK(SchemaToColumnPBs(projection, scan->mutable_projected_columns()));
  req.set_call_seq_id(0);
  req.set_batch_size_bytes(0); // so it won't return data right away

  {
    RpcController rpc;
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(proxy_->Scan(req, resp, &rpc));
    SCOPED_TRACE(SecureDebugString(*resp));
    ASSERT_FALSE(resp->has_error());
  }

  // Make sure that the chosen snapshot timestamp is sent back and
  // it is larger than the previous propagation timestamp.
  ASSERT_TRUE(resp->has_snap_timestamp());
  ASSERT_LT(propagated_timestamp, resp->snap_timestamp());
  // The 'propagated_timestamp' field must be set for 'success' responses.
  ASSERT_TRUE(resp->has_propagated_timestamp());
  ASSERT_TRUE(resp->has_more_results());
}

void TabletServerTest::DoOrderedScanTest(const Schema& projection,
                                         const string& expected_rows_as_string) {
  InsertTestRowsDirect(0, 10);
  ASSERT_OK(tablet_replica_->tablet()->Flush());
  InsertTestRowsDirect(10, 10);
  ASSERT_OK(tablet_replica_->tablet()->Flush());
  InsertTestRowsDirect(20, 10);

  ScanResponsePB resp;
  ScanRequestPB req;
  RpcController rpc;

  // Set up a new snapshot scan without a specified timestamp.
  NewScanRequestPB* scan = req.mutable_new_scan_request();
  scan->set_tablet_id(kTabletId);
  ASSERT_OK(SchemaToColumnPBs(projection, scan->mutable_projected_columns()));
  req.set_call_seq_id(0);
  scan->set_read_mode(READ_AT_SNAPSHOT);
  scan->set_order_mode(ORDERED);

  {
    SCOPED_TRACE(SecureDebugString(req));
    req.set_batch_size_bytes(0); // so it won't return data right away
    ASSERT_OK(proxy_->Scan(req, &resp, &rpc));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_FALSE(resp.has_error());
  }

  vector<string> results;
  ASSERT_NO_FATAL_FAILURE(
    DrainScannerToStrings(resp.scanner_id(), projection, &results));

  ASSERT_EQ(30, results.size());

  for (int i = 0; i < results.size(); ++i) {
    ASSERT_EQ(results[i], Substitute(expected_rows_as_string, i, i * 2));
  }
}

// Tests for KUDU-967. This test creates multiple row sets and then performs an ordered
// scan including the key columns in the projection but without marking them as keys.
// Without a fix for KUDU-967 the scan will often return out-of-order results.
TEST_F(TabletServerTest, TestOrderedScan_ProjectionWithKeyColumnsInOrder) {
  // Build a projection with all the columns, but don't mark the key columns as such.
  SchemaBuilder sb;
  for (int i = 0; i < schema_.num_columns(); i++) {
    sb.AddColumn(schema_.column(i), false);
  }
  const Schema& projection = sb.BuildWithoutIds();
  DoOrderedScanTest(projection,
                    R"((int32 key=$0, int32 int_val=$1, string string_val="hello $0"))");
}

// Same as above but doesn't add the key columns to the projection.
TEST_F(TabletServerTest, TestOrderedScan_ProjectionWithoutKeyColumns) {
  // Build a projection without the key columns.
  SchemaBuilder sb;
  for (int i = schema_.num_key_columns(); i < schema_.num_columns(); i++) {
    sb.AddColumn(schema_.column(i), false);
  }
  const Schema& projection = sb.BuildWithoutIds();
  DoOrderedScanTest(projection, R"((int32 int_val=$1, string string_val="hello $0"))");
}

// Same as above but creates a projection with the order of columns reversed.
TEST_F(TabletServerTest, TestOrderedScan_ProjectionWithKeyColumnsOutOfOrder) {
  // Build a projection with the order of the columns reversed.
  SchemaBuilder sb;
  for (int i = schema_.num_columns() - 1; i >= 0; i--) {
    sb.AddColumn(schema_.column(i), false);
  }
  const Schema& projection = sb.BuildWithoutIds();
  DoOrderedScanTest(projection,
                    R"((string string_val="hello $0", int32 int_val=$1, int32 key=$0))");
}

TEST_F(TabletServerTest, TestAlterSchema) {
  AlterSchemaRequestPB req;
  AlterSchemaResponsePB resp;
  RpcController rpc;

  InsertTestRowsRemote(0, 2);

  // Add one column with a default value
  const int32_t c2_write_default = 5;
  const int32_t c2_read_default = 7;
  SchemaBuilder builder(schema_);
  ASSERT_OK(builder.AddColumn("c2", INT32, false, &c2_read_default, &c2_write_default));
  Schema s2 = builder.Build();

  req.set_dest_uuid(mini_server_->server()->fs_manager()->uuid());
  req.set_tablet_id(kTabletId);
  req.set_schema_version(1);
  ASSERT_OK(SchemaToPB(s2, req.mutable_schema()));

  // Send the call
  {
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(admin_proxy_->AlterSchema(req, &resp, &rpc));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_FALSE(resp.has_error());
  }

  {
    InsertTestRowsRemote(2, 2);
    scoped_refptr<TabletReplica> tablet;
    ASSERT_TRUE(mini_server_->server()->tablet_manager()->LookupTablet(kTabletId, &tablet));
    ASSERT_OK(tablet->tablet()->Flush());
  }

  const Schema projection({ ColumnSchema("key", INT32), (ColumnSchema("c2", INT32)) }, 1);

  // Try recovering from the original log
  ASSERT_NO_FATAL_FAILURE(ShutdownAndRebuildTablet());
  VerifyRows(projection, { KeyValue(0, 7),
                           KeyValue(1, 7),
                           KeyValue(2, 5),
                           KeyValue(3, 5) });

  // Try recovering from the log generated on recovery
  ASSERT_NO_FATAL_FAILURE(ShutdownAndRebuildTablet());
  VerifyRows(projection, { KeyValue(0, 7),
                           KeyValue(1, 7),
                           KeyValue(2, 5),
                           KeyValue(3, 5) });
}

// Adds a new column with no "write default", and then restarts the tablet
// server. Inserts that were made before the new column was added should
// still replay properly during bootstrap.
//
// Regression test for KUDU-181.
TEST_F(TabletServerTest, TestAlterSchema_AddColWithoutWriteDefault) {
  AlterSchemaRequestPB req;
  AlterSchemaResponsePB resp;
  RpcController rpc;

  InsertTestRowsRemote(0, 2);

  // Add a column with a read-default but no write-default.
  const uint32_t c2_read_default = 7;
  SchemaBuilder builder(schema_);
  ASSERT_OK(builder.AddColumn("c2", INT32, false, &c2_read_default, nullptr));
  Schema s2 = builder.Build();

  req.set_dest_uuid(mini_server_->server()->fs_manager()->uuid());
  req.set_tablet_id(kTabletId);
  req.set_schema_version(1);
  ASSERT_OK(SchemaToPB(s2, req.mutable_schema()));

  // Send the call
  {
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(admin_proxy_->AlterSchema(req, &resp, &rpc));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_FALSE(resp.has_error());
  }

  // Verify that the old data picked up the read default.

  const Schema projection({ ColumnSchema("key", INT32), ColumnSchema("c2", INT32) }, 1);
  VerifyRows(projection, { KeyValue(0, 7), KeyValue(1, 7) });

  // Try recovering from the original log
  ASSERT_NO_FATAL_FAILURE(ShutdownAndRebuildTablet());
  VerifyRows(projection, { KeyValue(0, 7), KeyValue(1, 7) });

  // Try recovering from the log generated on recovery
  ASSERT_NO_FATAL_FAILURE(ShutdownAndRebuildTablet());
  VerifyRows(projection, { KeyValue(0, 7), KeyValue(1, 7) });
}

TEST_F(TabletServerTest, TestCreateTablet_TabletExists) {
  CreateTabletRequestPB req;
  CreateTabletResponsePB resp;
  RpcController rpc;

  req.set_dest_uuid(mini_server_->server()->fs_manager()->uuid());
  req.set_table_id("testtb");
  req.set_tablet_id(kTabletId);
  PartitionPB* partition = req.mutable_partition();
  partition->set_partition_key_start(" ");
  partition->set_partition_key_end(" ");
  req.set_table_name("testtb");
  req.mutable_config()->CopyFrom(mini_server_->CreateLocalConfig());

  Schema schema = SchemaBuilder(schema_).Build();
  ASSERT_OK(SchemaToPB(schema, req.mutable_schema()));

  // Send the call
  {
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(admin_proxy_->CreateTablet(req, &resp, &rpc));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_TRUE(resp.has_error());
    ASSERT_EQ(TabletServerErrorPB::TABLET_ALREADY_EXISTS, resp.error().code());
  }
}

TEST_F(TabletServerTest, TestDeleteTablet) {
  scoped_refptr<TabletReplica> tablet;

  // Verify that the tablet exists
  ASSERT_TRUE(mini_server_->server()->tablet_manager()->LookupTablet(kTabletId, &tablet));

  // Fetch the metric for the number of on-disk blocks, so we can later verify
  // that we actually remove data.
  scoped_refptr<AtomicGauge<uint64_t> > ondisk =
    METRIC_log_block_manager_blocks_under_management.Instantiate(
        mini_server_->server()->metric_entity(), 0);
  const int block_count_before_flush = ondisk->value();
  if (FLAGS_block_manager == "log") {
    ASSERT_EQ(block_count_before_flush, 0);
  }

  // Put some data in the tablet. We flush and insert more rows to ensure that
  // there is data both in the MRS and on disk.
  ASSERT_NO_FATAL_FAILURE(InsertTestRowsRemote(1, 1));
  ASSERT_OK(tablet_replica_->tablet()->Flush());
  ASSERT_NO_FATAL_FAILURE(InsertTestRowsRemote(2, 1));

  const int block_count_after_flush = ondisk->value();
  if (FLAGS_block_manager == "log") {
    ASSERT_GT(block_count_after_flush, block_count_before_flush);
  }

  // Drop any local references to the tablet from within this test,
  // so that when we delete it on the server, it's not held alive
  // by the test code.
  tablet_replica_.reset();
  tablet.reset();

  DeleteTabletRequestPB req;
  DeleteTabletResponsePB resp;
  RpcController rpc;

  req.set_dest_uuid(mini_server_->server()->fs_manager()->uuid());
  req.set_tablet_id(kTabletId);
  req.set_delete_type(tablet::TABLET_DATA_DELETED);

  // Send the call
  {
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(admin_proxy_->DeleteTablet(req, &resp, &rpc));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_FALSE(resp.has_error());
  }

  // Verify that the tablet is removed from the tablet map
  ASSERT_FALSE(mini_server_->server()->tablet_manager()->LookupTablet(kTabletId, &tablet));

  // Verify that fetching metrics doesn't crash. Regression test for KUDU-638.
  EasyCurl c;
  faststring buf;
  ASSERT_OK(c.FetchURL(strings::Substitute(
                                "http://$0/jsonmetricz",
                                mini_server_->bound_http_addr().ToString()),
                              &buf));

  // Verify data was actually removed.
  const int block_count_after_delete = ondisk->value();
  if (FLAGS_block_manager == "log") {
    ASSERT_EQ(block_count_after_delete, 0);
  }

  // Verify that after restarting the TS, the tablet is still not in the tablet manager.
  // This ensures that the on-disk metadata got removed.
  Status s = ShutdownAndRebuildTablet();
  ASSERT_TRUE(s.IsNotFound()) << s.ToString();
  ASSERT_FALSE(mini_server_->server()->tablet_manager()->LookupTablet(kTabletId, &tablet));
}

TEST_F(TabletServerTest, TestDeleteTablet_TabletNotCreated) {
  DeleteTabletRequestPB req;
  DeleteTabletResponsePB resp;
  RpcController rpc;

  req.set_dest_uuid(mini_server_->server()->fs_manager()->uuid());
  req.set_tablet_id("NotPresentTabletId");
  req.set_delete_type(tablet::TABLET_DATA_DELETED);

  // Send the call
  {
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(admin_proxy_->DeleteTablet(req, &resp, &rpc));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_TRUE(resp.has_error());
    ASSERT_EQ(TabletServerErrorPB::TABLET_NOT_FOUND, resp.error().code());
  }
}

TEST_F(TabletServerTest, TestDeleteTabletBenchmark) {
  // Collect some related metrics.
  scoped_refptr<AtomicGauge<uint64_t>> block_count =
      METRIC_log_block_manager_blocks_under_management.Instantiate(
          mini_server_->server()->metric_entity(), 0);
  scoped_refptr<AtomicGauge<uint64_t>> container =
      METRIC_log_block_manager_containers.Instantiate(
          mini_server_->server()->metric_entity(), 0);
  scoped_refptr<Counter> holes_punched =
      METRIC_log_block_manager_holes_punched.Instantiate(
          mini_server_->server()->metric_entity());

  // Put some data in the tablet. We insert rows and flush immediately to
  // ensure that there is enough blocks on disk to run the benchmark.
  for (int i = 0; i < FLAGS_delete_tablet_bench_num_flushes; i++) {
    NO_FATALS(InsertTestRowsRemote(i, 1));
    ASSERT_OK(tablet_replica_->tablet()->Flush());
  }
  const int block_count_before_delete = block_count->value();

  // Drop any local references to the tablet from within this test,
  // so that when we delete it on the server, it's not held alive
  // by the test code.
  tablet_replica_.reset();

  DeleteTabletRequestPB req;
  DeleteTabletResponsePB resp;
  RpcController rpc;

  req.set_dest_uuid(mini_server_->server()->fs_manager()->uuid());
  req.set_tablet_id(kTabletId);
  req.set_delete_type(tablet::TABLET_DATA_DELETED);

  // Send the call and measure the time spent deleting the tablet.
  LOG_TIMING(INFO, "deleting tablet") {
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(admin_proxy_->DeleteTablet(req, &resp, &rpc));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_FALSE(resp.has_error());
  }

  // Log the related metrics.
  LOG(INFO) << "block_count_before_delete : " << block_count_before_delete;
  LOG(INFO) << "log_block_manager_containers : " << container->value();
  LOG(INFO) << "log_block_manager_holes_punched : " << holes_punched->value();
}

// Test that with concurrent requests to delete the same tablet, one wins and
// the other fails, with no assertion failures. Regression test for KUDU-345.
TEST_F(TabletServerTest, TestConcurrentDeleteTablet) {
  // Verify that the tablet exists
  scoped_refptr<TabletReplica> tablet;
  ASSERT_TRUE(mini_server_->server()->tablet_manager()->LookupTablet(kTabletId, &tablet));

  static const int kNumDeletes = 2;
  RpcController rpcs[kNumDeletes];
  DeleteTabletResponsePB responses[kNumDeletes];
  CountDownLatch latch(kNumDeletes);

  DeleteTabletRequestPB req;
  req.set_dest_uuid(mini_server_->server()->fs_manager()->uuid());
  req.set_tablet_id(kTabletId);
  req.set_delete_type(tablet::TABLET_DATA_DELETED);

  for (int i = 0; i < kNumDeletes; i++) {
    SCOPED_TRACE(SecureDebugString(req));
    admin_proxy_->DeleteTabletAsync(req, &responses[i], &rpcs[i],
                                    boost::bind(&CountDownLatch::CountDown, &latch));
  }
  latch.Wait();

  int num_success = 0;
  for (int i = 0; i < kNumDeletes; i++) {
    ASSERT_TRUE(rpcs[i].finished());
    LOG(INFO) << "STATUS " << i << ": " << rpcs[i].status().ToString();
    LOG(INFO) << "RESPONSE " << i << ": " << SecureDebugString(responses[i]);
    if (!responses[i].has_error()) {
      num_success++;
    }
  }

  // Verify that the tablet is removed from the tablet map
  ASSERT_FALSE(mini_server_->server()->tablet_manager()->LookupTablet(kTabletId, &tablet));
  ASSERT_EQ(1, num_success);
}

TEST_F(TabletServerTest, TestInsertLatencyMicroBenchmark) {
  METRIC_DEFINE_entity(test);
  METRIC_DEFINE_histogram(test, insert_latency,
                          "Insert Latency",
                          MetricUnit::kMicroseconds,
                          "TabletServer single threaded insert latency.",
                          10000000,
                          2);

  scoped_refptr<Histogram> histogram = METRIC_insert_latency.Instantiate(ts_test_metric_entity_);

  uint64_t warmup = AllowSlowTests() ?
      FLAGS_single_threaded_insert_latency_bench_warmup_rows : 10;

  for (int i = 0; i < warmup; i++) {
    InsertTestRowsRemote(i, 1);
  }

  uint64_t max_rows = AllowSlowTests() ?
      FLAGS_single_threaded_insert_latency_bench_insert_rows : 100;

  MonoTime start = MonoTime::Now();

  for (int i = warmup; i < warmup + max_rows; i++) {
    MonoTime before = MonoTime::Now();
    InsertTestRowsRemote(i, 1);
    MonoTime after = MonoTime::Now();
    MonoDelta delta = after - before;
    histogram->Increment(delta.ToMicroseconds());
  }

  MonoTime end = MonoTime::Now();
  double throughput = ((max_rows - warmup) * 1.0) / (end - start).ToSeconds();

  // Generate the JSON.
  std::ostringstream out;
  JsonWriter writer(&out, JsonWriter::PRETTY);
  ASSERT_OK(histogram->WriteAsJson(&writer, MetricJsonOptions()));

  LOG(INFO) << "Throughput: " << throughput << " rows/sec.";
  LOG(INFO) << out.str();
}

// Simple test to ensure we can destroy an RpcServer in different states of
// initialization before Start()ing it.
TEST_F(TabletServerTest, TestRpcServerCreateDestroy) {
  RpcServerOptions opts;
  {
    RpcServer server(opts);
  }
  {
    RpcServer server(opts);
    MessengerBuilder mb("foo");
    shared_ptr<Messenger> messenger;
    ASSERT_OK(mb.Build(&messenger));
    ASSERT_OK(server.Init(messenger));
  }
}

TEST_F(TabletServerTest, TestWriteOutOfBounds) {
  const char *tabletId = "TestWriteOutOfBoundsTablet";
  Schema schema = SchemaBuilder(schema_).Build();

  PartitionSchema partition_schema;
  CHECK_OK(PartitionSchema::FromPB(PartitionSchemaPB(), schema, &partition_schema));

  KuduPartialRow start_row(&schema);
  ASSERT_OK(start_row.SetInt32("key", 10));

  KuduPartialRow end_row(&schema);
  ASSERT_OK(end_row.SetInt32("key", 20));

  vector<Partition> partitions;
  ASSERT_OK(partition_schema.CreatePartitions({ start_row, end_row }, {}, schema, &partitions));

  ASSERT_EQ(3, partitions.size());

  ASSERT_OK(mini_server_->server()->tablet_manager()->CreateNewTablet(
      "TestWriteOutOfBoundsTable", tabletId,
      partitions[1],
      tabletId, schema, partition_schema,
      mini_server_->CreateLocalConfig(), nullptr));

  ASSERT_OK(WaitForTabletRunning(tabletId));

  WriteRequestPB req;
  WriteResponsePB resp;
  RpcController controller;
  req.set_tablet_id(tabletId);
  ASSERT_OK(SchemaToPB(schema_, req.mutable_schema()));

  vector<RowOperationsPB::Type> ops = { RowOperationsPB::INSERT, RowOperationsPB::UPDATE };

  for (const RowOperationsPB::Type &op : ops) {
    RowOperationsPB* data = req.mutable_row_operations();
    AddTestRowToPB(op, schema_, 20, 1, "1", data);
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(SecureDebugString(resp));

    ASSERT_TRUE(resp.has_error());
    ASSERT_EQ(TabletServerErrorPB::UNKNOWN_ERROR, resp.error().code());
    Status s = StatusFromPB(resp.error().status());
    EXPECT_TRUE(s.IsNotFound());
    ASSERT_STR_CONTAINS(s.ToString(),
                        "Not found: Row not in tablet partition");
    data->Clear();
    controller.Reset();
  }
}

static uint32_t CalcTestRowChecksum(int32_t key, uint8_t string_field_defined = true) {
  crc::Crc* crc = crc::GetCrc32cInstance();
  uint64_t row_crc = 0;

  string strval = strings::Substitute("original$0", key);
  uint32_t index = 0;
  crc->Compute(&index, sizeof(index), &row_crc, nullptr);
  crc->Compute(&key, sizeof(int32_t), &row_crc, nullptr);

  index = 1;
  crc->Compute(&index, sizeof(index), &row_crc, nullptr);
  crc->Compute(&key, sizeof(int32_t), &row_crc, nullptr);

  index = 2;
  crc->Compute(&index, sizeof(index), &row_crc, nullptr);
  crc->Compute(&string_field_defined, sizeof(string_field_defined), &row_crc, nullptr);
  if (string_field_defined) {
    crc->Compute(strval.c_str(), strval.size(), &row_crc, nullptr);
  }
  return static_cast<uint32_t>(row_crc);
}

// Simple test to check that our checksum scans work as expected.
TEST_F(TabletServerTest, TestChecksumScan) {
  uint64_t total_crc = 0;

  ChecksumRequestPB req;
  req.mutable_new_request()->set_tablet_id(kTabletId);
  req.mutable_new_request()->set_read_mode(READ_LATEST);
  req.set_call_seq_id(0);
  ASSERT_OK(SchemaToColumnPBs(schema_, req.mutable_new_request()->mutable_projected_columns(),
                              SCHEMA_PB_WITHOUT_IDS));
  ChecksumRequestPB new_req = req;  // Cache "new" request.

  ChecksumResponsePB resp;
  RpcController controller;
  ASSERT_OK(proxy_->Checksum(req, &resp, &controller));

  // No rows.
  ASSERT_EQ(total_crc, resp.checksum());
  ASSERT_FALSE(resp.has_more_results());

  // First row.
  int32_t key = 1;
  InsertTestRowsRemote(key, 1);
  controller.Reset();
  ASSERT_OK(proxy_->Checksum(req, &resp, &controller));
  total_crc += CalcTestRowChecksum(key);
  uint64_t first_crc = total_crc; // Cache first record checksum.

  ASSERT_FALSE(resp.has_error()) << SecureDebugString(resp.error());
  ASSERT_EQ(total_crc, resp.checksum());
  ASSERT_FALSE(resp.has_more_results());
  EXPECT_TRUE(resp.has_resource_metrics());
  EXPECT_EQ(1, resp.rows_checksummed());

  // Second row (null string field).
  key = 2;
  InsertTestRowsRemote(key, 1, 1, nullptr, kTabletId, nullptr, nullptr, false);
  controller.Reset();
  ASSERT_OK(proxy_->Checksum(req, &resp, &controller));
  total_crc += CalcTestRowChecksum(key, false);

  ASSERT_FALSE(resp.has_error()) << SecureDebugString(resp.error());
  ASSERT_EQ(total_crc, resp.checksum());
  ASSERT_FALSE(resp.has_more_results());

  // Now test the same thing, but with a scan requiring 2 passes (one per row).
  FLAGS_scanner_batch_size_rows = 1;
  req.set_batch_size_bytes(1);
  controller.Reset();
  ASSERT_OK(proxy_->Checksum(req, &resp, &controller));
  string scanner_id = resp.scanner_id();
  ASSERT_TRUE(resp.has_more_results());
  uint64_t agg_checksum = resp.checksum();

  // Second row.
  req.clear_new_request();
  req.mutable_continue_request()->set_scanner_id(scanner_id);
  req.mutable_continue_request()->set_previous_checksum(agg_checksum);
  req.set_call_seq_id(1);
  controller.Reset();
  ASSERT_OK(proxy_->Checksum(req, &resp, &controller));
  ASSERT_EQ(total_crc, resp.checksum());
  ASSERT_FALSE(resp.has_more_results());

  // Finally, delete row 2, so we're back to the row 1 checksum.
  ASSERT_NO_FATAL_FAILURE(DeleteTestRowsRemote(key, 1));
  FLAGS_scanner_batch_size_rows = 100;
  req = new_req;
  controller.Reset();
  ASSERT_OK(proxy_->Checksum(req, &resp, &controller));
  ASSERT_NE(total_crc, resp.checksum());
  ASSERT_EQ(first_crc, resp.checksum());
  ASSERT_FALSE(resp.has_more_results());
}

class DelayFsyncLogHook : public log::Log::LogFaultHooks {
 public:
  DelayFsyncLogHook() : log_latch1_(1), test_latch1_(1) {}

  Status PostAppend() override {
    test_latch1_.CountDown();
    log_latch1_.Wait();
    log_latch1_.Reset(1);
    return Status::OK();
  }

  void Continue() {
    test_latch1_.Wait();
    log_latch1_.CountDown();
  }

 private:
  CountDownLatch log_latch1_;
  CountDownLatch test_latch1_;
};

namespace {

void DeleteOneRowAsync(TabletServerTest* test) {
  test->DeleteTestRowsRemote(10, 1);
}

void CompactAsync(Tablet* tablet, CountDownLatch* flush_done_latch) {
  CHECK_OK(tablet->Compact(Tablet::FORCE_COMPACT_ALL));
  flush_done_latch->CountDown();
}

} // namespace

// Tests that in flight transactions are committed and that commit messages
// are durable before a compaction is allowed to flush the tablet metadata.
//
// This test is in preparation for KUDU-120 and should pass before and after
// it, but was also confirmed to fail if the pre-conditions it tests for
// fail. That is if KUDU-120 is implemented without these pre-requisites
// this test is confirmed to fail.
TEST_F(TabletServerTest, TestKudu120PreRequisites) {

  // Insert a few rows...
  InsertTestRowsRemote(0, 10);
  // ... now flush ...
  ASSERT_OK(tablet_replica_->tablet()->Flush());
  // ... insert a few rows...
  InsertTestRowsRemote(10, 10);
  // ... and flush again so that we have two disk row sets.
  ASSERT_OK(tablet_replica_->tablet()->Flush());

  // Add a hook so that we can make the log wait right after an append
  // (before the callback is triggered).
  log::Log* log = tablet_replica_->log();
  shared_ptr<DelayFsyncLogHook> log_hook(new DelayFsyncLogHook);
  log->SetLogFaultHooksForTests(log_hook);

  // Now start a transaction (delete) and stop just before commit.
  scoped_refptr<kudu::Thread> thread1;
  CHECK_OK(kudu::Thread::Create("DeleteThread", "DeleteThread",
                                DeleteOneRowAsync, this, &thread1));

  // Wait for the replicate message to arrive and continue.
  log_hook->Continue();
  // Wait a few msecs to make sure that the transaction is
  // trying to commit.
  usleep(100* 1000); // 100 msecs

  // Now start a compaction before letting the commit message go through.
  scoped_refptr<kudu::Thread> flush_thread;
  CountDownLatch flush_done_latch(1);
  CHECK_OK(kudu::Thread::Create("CompactThread", "CompactThread",
                                CompactAsync,
                                tablet_replica_->tablet(),
                                &flush_done_latch,
                                &flush_thread));

  // At this point we have both a compaction and a transaction going on.
  // If we allow the transaction to return before the commit message is
  // durable (KUDU-120) that means that the mvcc transaction will no longer
  // be in flight at this moment, nonetheless since we're blocking the WAL
  // and not allowing the commit message to go through, the compaction should
  // be forced to wait.
  //
  // We are thus testing two conditions:
  // - That in-flight transactions are committed.
  // - That commit messages for transactions that were in flight are durable.
  //
  // If these pre-conditions are not met, i.e. if the compaction is not forced
  // to wait here for the conditions to be true, then the below assertion
  // will fail, since the transaction's commit write callback will only
  // return when we allow it (in log_hook->Continue());
  CHECK(!flush_done_latch.WaitFor(MonoDelta::FromMilliseconds(300)));

  // Now let the rest go through.
  log_hook->Continue();
  log_hook->Continue();
  flush_done_latch.Wait();
}

// Test DNS resolution failure in the master heartbeater.
// Regression test for KUDU-1681.
TEST_F(TabletServerTest, TestFailedDnsResolution) {
  FLAGS_fail_dns_resolution = true;
  mini_server_->server()->heartbeater()->TriggerASAP();
  // Wait to make sure the heartbeater thread attempts the DNS lookup.
  usleep(100 * 1000);
}

TEST_F(TabletServerTest, TestDataDirGroupsCreated) {
  // Get the original superblock.
  TabletSuperBlockPB superblock;
  tablet_replica_->tablet()->metadata()->ToSuperBlock(&superblock);
  DataDirGroupPB orig_group = superblock.data_dir_group();

  // Remove the DataDirGroupPB on-disk.
  superblock.clear_data_dir_group();
  ASSERT_FALSE(superblock.has_data_dir_group());
  string tablet_meta_path = JoinPathSegments(GetTestPath("TabletServerTest-fsroot"), "tablet-meta");
  string pb_path = JoinPathSegments(tablet_meta_path, tablet_replica_->tablet_id());
  ASSERT_OK(pb_util::WritePBContainerToPath(Env::Default(),
      pb_path, superblock, pb_util::OVERWRITE, pb_util::SYNC));

  // Verify that the on-disk copy has its DataDirGroup missing.
  ASSERT_OK(tablet_replica_->tablet()->metadata()->ReadSuperBlockFromDisk(&superblock));
  ASSERT_FALSE(superblock.has_data_dir_group());

  // Restart the server and check that a new group is created. By default, the
  // group will be created with all data directories and should be identical to
  // the original one.
  ASSERT_OK(ShutdownAndRebuildTablet());
  tablet_replica_->tablet()->metadata()->ToSuperBlock(&superblock);
  DataDirGroupPB new_group = superblock.data_dir_group();
  MessageDifferencer md;
  ASSERT_TRUE(md.Compare(orig_group, new_group));
}

TEST_F(TabletServerTest, TestNoMetricsForTombstonedTablet) {
  // Force the metrics to be retired immediately.
  FLAGS_metrics_retirement_age_ms = 0;

  scoped_refptr<TabletReplica> tablet;
  ASSERT_TRUE(mini_server_->server()->tablet_manager()->LookupTablet(kTabletId, &tablet));

  // Insert one row and check the insertion is recorded in the metrics.
  ASSERT_NO_FATAL_FAILURE(InsertTestRowsRemote(0, 1, 1));
  scoped_refptr<Counter> rows_inserted =
      METRIC_rows_inserted.Instantiate(tablet->tablet()->GetMetricEntity());
  int64_t num_rows_running = rows_inserted->value();
  ASSERT_EQ(1, num_rows_running);

  // Tombstone the tablet.
  DeleteTabletRequestPB req;
  DeleteTabletResponsePB resp;
  RpcController rpc;
  req.set_dest_uuid(mini_server_->server()->fs_manager()->uuid());
  req.set_tablet_id(kTabletId);
  req.set_delete_type(tablet::TABLET_DATA_TOMBSTONED);
  {
    SCOPED_TRACE(SecureDebugString(req));
    ASSERT_OK(admin_proxy_->DeleteTablet(req, &resp, &rpc));
    SCOPED_TRACE(SecureDebugString(resp));
    ASSERT_FALSE(resp.has_error());
  }

  // It takes three calls to /jsonmetricz for the tablet metrics to go away, based on the
  // policy in MetricRegistry::RetireOldMetrics:
  // 1. The entity's metrics are returned, but also marked for retirement.
  // 2. The entity's metrics are returned, but also retired (causing the entity to be retired).
  // 3. The metrics aren't returned-- the entity has been removed from the metrics registry.
  EasyCurl c;
  faststring buf;
  for (int i = 0; i < 3; i++) {
    ASSERT_OK(c.FetchURL(strings::Substitute("http://$0/jsonmetricz",
                                             mini_server_->bound_http_addr().ToString()),
                         &buf));
    if (i < 2) {
      ASSERT_STR_CONTAINS(buf.ToString(), "\"type\": \"tablet\"");
    } else {
      ASSERT_STR_NOT_CONTAINS(buf.ToString(), "\"type\": \"tablet\"");
    }
  }
}

} // namespace tserver
} // namespace kudu
