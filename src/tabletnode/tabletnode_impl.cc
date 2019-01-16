// Copyright (c) 2015, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tabletnode/tabletnode_impl.h"

#include <algorithm>
#include <functional>
#include <set>
#include <thread>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gperftools/malloc_extension.h>

#include "db/filename.h"
#include "db/table_cache.h"
#include "common/base/string_ext.h"
#include "common/metric/cache_collector.h"
#include "common/metric/tcmalloc_collector.h"
#include "common/metric/prometheus_subscriber.h"
#include "common/metric/ratio_collector.h"
#include "common/metric/metric_counter.h"
#include "io/io_utils.h"
#include "io/utils_leveldb.h"
#include "leveldb/cache.h"
#include "leveldb/env_dfs.h"
#include "leveldb/env_flash.h"
#include "leveldb/env_inmem.h"
#include "leveldb/config.h"
#include "leveldb/slog.h"
#include "leveldb/table_utils.h"
#include "leveldb/util/stop_watch.h"
#include "leveldb/util/dfs_read_thread_limiter.h"
#include "proto/kv_helper.h"
#include "proto/proto_helper.h"
#include "proto/tabletnode_client.h"
#include "tabletnode/tablet_manager.h"
#include "tabletnode/tabletnode_metric_name.h"
#include "tabletnode/tabletnode_zk_adapter.h"
#include "types.h"
#include "utils/config_utils.h"
#include "common/counter.h"
#include "utils/string_util.h"
#include "common/timer.h"
#include "utils/utils_cmd.h"

DECLARE_string(tera_tabletnode_port);
DECLARE_int64(tera_heartbeat_period);
DECLARE_int64(tera_heartbeat_retry_period_factor);
DECLARE_int32(tera_heartbeat_retry_times);
DECLARE_bool(tera_tabletnode_tcm_cache_release_enabled);
DECLARE_int32(tera_tabletnode_tcm_cache_release_period);

DECLARE_int32(tera_tabletnode_impl_thread_max_num);

DECLARE_bool(tera_zk_enabled);
DECLARE_bool(tera_mock_zk_enabled);

DECLARE_string(tera_master_meta_table_name);
DECLARE_int32(tera_tabletnode_retry_period);
DECLARE_string(tera_leveldb_log_path);
DECLARE_int32(leveldb_max_log_size_MB);
DECLARE_int32(leveldb_log_flush_trigger_size_B);
DECLARE_int32(leveldb_log_flush_trigger_interval_ms);

DECLARE_bool(tera_tabletnode_rpc_limit_enabled);
DECLARE_int32(tera_tabletnode_rpc_limit_max_inflow);
DECLARE_int32(tera_tabletnode_rpc_limit_max_outflow);
DECLARE_int32(tera_tabletnode_rpc_max_pending_buffer_size);
DECLARE_int32(tera_tabletnode_rpc_work_thread_num);
DECLARE_int32(tera_tabletnode_scan_pack_max_size);
DECLARE_int32(tera_tabletnode_block_cache_size);
DECLARE_int32(tera_tabletnode_table_cache_size);
DECLARE_int32(tera_tabletnode_compact_thread_num);
DECLARE_string(tera_tabletnode_path_prefix);

// cache-related
DECLARE_int32(tera_memenv_block_cache_size);
DECLARE_bool(tera_tabletnode_delete_old_flash_cache_enabled);
DECLARE_int64(meta_block_cache_size);
DECLARE_int64(meta_table_cache_size);
DECLARE_string(tera_tabletnode_cache_paths);
DECLARE_int32(tera_tabletnode_cache_block_size);
DECLARE_string(tera_tabletnode_cache_name);
DECLARE_int32(tera_tabletnode_cache_mem_size);
DECLARE_int32(tera_tabletnode_cache_disk_size);
DECLARE_int32(tera_tabletnode_cache_disk_filenum);
DECLARE_int32(tera_tabletnode_cache_log_level);
DECLARE_int32(tera_tabletnode_cache_update_thread_num);
DECLARE_bool(tera_tabletnode_cache_force_read_from_cache);
DECLARE_int32(tera_tabletnode_gc_log_level);

DECLARE_string(tera_leveldb_env_type);
DECLARE_string(tera_local_addr);
DECLARE_bool(tera_ins_enabled);
DECLARE_bool(tera_mock_ins_enabled);
DECLARE_string(tera_coord_type);

DECLARE_bool(tera_io_cache_path_vanish_allowed);
DECLARE_int64(tera_tabletnode_tcm_cache_size);
DECLARE_int64(tera_refresh_tablets_status_interval_ms);

DECLARE_string(flagfile);

DECLARE_int64(tera_tabletnode_parallel_read_task_num);
DECLARE_int64(tera_tabletnode_parallel_read_rows_per_task);
DECLARE_bool(tera_enable_persistent_cache);
DECLARE_bool(tera_tabletnode_clean_persistent_cache_paths);

DECLARE_int32(tera_tabletnode_read_thread_num);
DECLARE_double(dfs_read_thread_ratio);

using namespace std::placeholders;

static const int GC_LOG_LEVEL = FLAGS_tera_tabletnode_gc_log_level;

namespace leveldb {
extern tera::Counter snappy_before_size_counter;
extern tera::Counter snappy_after_size_counter;
}

namespace tera {
namespace tabletnode {
using tera::SubscriberType;
using std::make_shared;

tera::MetricCounter read_error_counter(kErrorCountMetric, kApiLabelRead,
                                       {SubscriberType::QPS, SubscriberType::SUM});
tera::MetricCounter write_error_counter(kErrorCountMetric, kApiLabelWrite,
                                        {SubscriberType::QPS, SubscriberType::SUM});
tera::MetricCounter scan_error_counter(kErrorCountMetric, kApiLabelScan,
                                       {SubscriberType::QPS, SubscriberType::SUM});

tera::MetricCounter read_range_error_counter(kRangeErrorMetric, kApiLabelRead,
                                             {SubscriberType::QPS});
tera::MetricCounter write_range_error_counter(kRangeErrorMetric, kApiLabelWrite,
                                              {SubscriberType::QPS});
tera::MetricCounter scan_range_error_counter(kRangeErrorMetric, kApiLabelScan,
                                             {SubscriberType::QPS});

extern tera::MetricCounter read_reject_counter;

TabletNodeImpl::CacheMetrics::CacheMetrics(leveldb::Cache* block_cache,
                                           leveldb::TableCache* table_cache)
    : block_cache_hitrate_(kBlockCacheHitRateMetric,
                           std::unique_ptr<Collector>(
                               new LRUCacheCollector(block_cache, CacheCollectType::kHitRate))),
      block_cache_entries_(kBlockCacheEntriesMetric,
                           std::unique_ptr<Collector>(
                               new LRUCacheCollector(block_cache, CacheCollectType::kEntries))),
      block_cache_charge_(kBlockCacheChargeMetric, std::unique_ptr<Collector>(new LRUCacheCollector(
                                                       block_cache, CacheCollectType::kCharge))),
      table_cache_hitrate_(kTableCacheHitRateMetric,
                           std::unique_ptr<Collector>(
                               new TableCacheCollector(table_cache, CacheCollectType::kHitRate))),
      table_cache_entries_(kTableCacheEntriesMetric,
                           std::unique_ptr<Collector>(
                               new TableCacheCollector(table_cache, CacheCollectType::kEntries))),
      table_cache_charge_(kTableCacheChargeMetric,
                          std::unique_ptr<Collector>(
                              new TableCacheCollector(table_cache, CacheCollectType::kCharge))) {}

TabletNodeImpl::TabletNodeImpl()
    : status_(kNotInited),
      running_(true),
      tablet_manager_(new TabletManager()),
      zk_adapter_(NULL),
      release_cache_timer_id_(kInvalidTimerId),
      thread_pool_(new ThreadPool(FLAGS_tera_tabletnode_impl_thread_max_num)),
      cache_metrics_(NULL) {
  if (FLAGS_tera_local_addr == "") {
    local_addr_ = utils::GetLocalHostName() + ":" + FLAGS_tera_tabletnode_port;
  } else {
    local_addr_ = FLAGS_tera_local_addr + ":" + FLAGS_tera_tabletnode_port;
  }
  sysinfo_.SetServerAddr(local_addr_);

  leveldb::Env::Default()->SetBackgroundThreads(FLAGS_tera_tabletnode_compact_thread_num);

  uint64_t max_log_size = static_cast<uint64_t>(FLAGS_leveldb_max_log_size_MB) << 20;
  leveldb::LogOption log_opt =
      leveldb::LogOption::LogOptionBuilder()
          .SetMaxLogSize(max_log_size)
          .SetFlushTriggerSize(FLAGS_leveldb_log_flush_trigger_size_B)
          .SetFlushTriggerIntervalMs(FLAGS_leveldb_log_flush_trigger_interval_ms)
          .Build();
  leveldb::Status s =
      leveldb::Env::Default()->NewLogger(FLAGS_tera_leveldb_log_path, log_opt, &ldb_logger_);
  leveldb::Env::Default()->SetLogger(ldb_logger_);
  LOG(INFO) << "leveldb logger inited, log_file:" << FLAGS_tera_leveldb_log_path
            << ", options:" << log_opt.ToString();

  ldb_block_cache_ = leveldb::NewLRUCache(FLAGS_tera_tabletnode_block_cache_size * 1024UL * 1024);
  m_memory_cache = leveldb::NewLRUCache(FLAGS_tera_memenv_block_cache_size * 1024UL * 1024);
  ldb_table_cache_ =
      new leveldb::TableCache(FLAGS_tera_tabletnode_table_cache_size * 1024UL * 1024);
  if (!s.ok()) {
    ldb_logger_ = NULL;
  }

  if (FLAGS_tera_leveldb_env_type != "local") {
    io::InitDfsEnv();
  }

  if (FLAGS_tera_tabletnode_tcm_cache_release_enabled) {
    LOG(INFO) << "enable tcmalloc cache release timer";
    EnableReleaseMallocCacheTimer();
  }
  const char* tcm_property = "tcmalloc.max_total_thread_cache_bytes";
  MallocExtension::instance()->SetNumericProperty(tcm_property,
                                                  FLAGS_tera_tabletnode_tcm_cache_size);
  size_t tcm_t;
  CHECK(MallocExtension::instance()->GetNumericProperty(tcm_property, &tcm_t));
  LOG(INFO) << tcm_property << "=" << tcm_t;
  sysinfo_.SetProcessStartTime(get_micros());
  for (int level = 0; level != leveldb::config::kNumLevels; ++level) {
    level_size_.push_back(tera::MetricCounter{
        kLevelSize, "level:" + std::to_string(level), {tera::SubscriberType::LATEST}, false});
    level_size_.back().Set(0);
  }
}

TabletNodeImpl::~TabletNodeImpl() {}

bool TabletNodeImpl::Init() {
  if (!InitCacheSystem()) {
    LOG(ERROR) << "Init cache system failed, exit.";
    return false;
  }

  InitDfsReadThreadLimiter();

  if (FLAGS_tera_coord_type.empty()) {
    LOG(ERROR) << "Note: We don't recommend that use '"
               << "--tera_[zk|ins|mock_zk|mock_ins]_enabled' flag for your cluster "
                  "coord"
               << " replace by '--tera_coord_type=[zk|ins|mock_zk|mock_ins|fake_zk]'"
               << " flag is usually recommended.";
  }
  if (FLAGS_tera_coord_type == "zk" || (FLAGS_tera_coord_type.empty() && FLAGS_tera_zk_enabled)) {
    zk_adapter_.reset(new TabletNodeZkAdapter(this, local_addr_));
  } else if (FLAGS_tera_coord_type == "ins" ||
             (FLAGS_tera_coord_type.empty() && FLAGS_tera_ins_enabled)) {
    LOG(INFO) << "ins mode!";
    zk_adapter_.reset(new InsTabletNodeZkAdapter(this, local_addr_));
  } else if (FLAGS_tera_coord_type == "mock_zk" ||
             (FLAGS_tera_coord_type.empty() && FLAGS_tera_mock_zk_enabled)) {
    LOG(INFO) << "mock zk mode!";
    zk_adapter_.reset(new MockTabletNodeZkAdapter(this, local_addr_));
  } else if (FLAGS_tera_coord_type == "mock_ins" ||
             (FLAGS_tera_coord_type.empty() && FLAGS_tera_mock_ins_enabled)) {
    LOG(INFO) << "mock ins mode!";
    zk_adapter_.reset(new MockInsTabletNodeZkAdapter(this, local_addr_));
  } else if (FLAGS_tera_coord_type == "fake_zk" || FLAGS_tera_coord_type.empty()) {
    LOG(INFO) << "fake zk mode!";
    zk_adapter_.reset(new FakeTabletNodeZkAdapter(this, local_addr_));
  }

  SetTabletNodeStatus(kIsIniting);
  thread_pool_->AddTask(std::bind(&TabletNodeZkAdapterBase::Init, zk_adapter_.get()));

  // register cache metrics
  cache_metrics_.reset(new CacheMetrics(ldb_block_cache_, ldb_table_cache_));
  RegisterTcmallocCollectors();
  // register snappy metrics
  snappy_ratio_metric_.reset(new AutoCollectorRegister(
      kSnappyCompressionRatioMetric,
      std::unique_ptr<Collector>(new RatioCollector(&leveldb::snappy_before_size_counter,
                                                    &leveldb::snappy_after_size_counter, true))));

  // update tablets status at background
  tablet_healthcheck_thread_ = std::thread{&TabletNodeImpl::RefreshTabletsStatus, this};
  return true;
}

bool TabletNodeImpl::InitCacheSystem() {
  sysinfo_.SetPersistentCacheSize(0);
  if (!io::GetCachePaths().empty()) {
    if (FLAGS_tera_enable_persistent_cache) {
      LOG(INFO) << "Enable persistent cache.";
      std::shared_ptr<leveldb::PersistentCache> persistent_cache;
      auto status = ::tera::io::GetPersistentCache(&persistent_cache);

      if (!status.ok() || !persistent_cache) {
        LOG(ERROR) << "Init persistent cache failed: " << status.ToString() << ", exit on error.";
        return false;
      }
      sysinfo_.SetPersistentCacheSize(persistent_cache->GetCapacity());
    } else {
      LOG(INFO) << "Enable flash env.";
      // compitable with legacy FlashEnv
      leveldb::FlashEnv* flash_env = (leveldb::FlashEnv*)io::LeveldbFlashEnv();
      if (FLAGS_tera_tabletnode_clean_persistent_cache_paths) {
        for (const auto& path : io::GetCachePaths()) {
          auto s = leveldb::Env::Default()->DeleteDirRecursive(path);
          if (!s.ok()) {
            LOG(WARNING) << "Remove persistent cache paths " << path << " failed: " << s.ToString();
          }
        }
      }
      assert(flash_env);
      flash_env->SetFlashPath(FLAGS_tera_tabletnode_cache_paths,
                              FLAGS_tera_io_cache_path_vanish_allowed);
      flash_env->SetUpdateFlashThreadNumber(FLAGS_tera_tabletnode_cache_update_thread_num);
      flash_env->SetIfForceReadFromCache(FLAGS_tera_tabletnode_cache_force_read_from_cache);
      flash_env->TryRollbackPersistentCacheFiles();
    }
  }
  return true;
}

bool TabletNodeImpl::Exit() {
  running_ = false;
  exit_event_.Set();

  cache_metrics_.reset(NULL);

  std::vector<io::TabletIO*> tablet_ios;
  tablet_manager_->GetAllTablets(&tablet_ios);

  std::vector<std::thread> unload_threads;
  unload_threads.reserve(tablet_ios.size());

  Counter worker_count;
  worker_count.Set(tablet_ios.size());

  for (uint32_t i = 0; i < tablet_ios.size(); ++i) {
    io::TabletIO* tablet_io = tablet_ios[i];
    unload_threads.emplace_back(
        std::bind(&TabletNodeImpl::UnloadTabletProc, this, tablet_io, &worker_count));
  }
  int64_t print_ms_ = get_millis();
  int64_t left = 0;
  while ((left = worker_count.Get()) > 0) {
    if (get_millis() - print_ms_ > 1000) {
      LOG(INFO) << "[Exit] " << left << " tablets are still unloading ...";
      print_ms_ = get_millis();
    }
    ThisThread::Sleep(100);
  }
  for (uint32_t i = 0; i < tablet_ios.size(); ++i) {
    unload_threads[i].join();
  }
  tablet_healthcheck_thread_.join();

  std::unique_ptr<leveldb::Logger> logger(leveldb::Logger::DefaultLogger());
  if (logger) {
    logger->Exit();
  }

  zk_adapter_->Exit();
  return true;
}

void TabletNodeImpl::RefreshTabletsStatus() {
  while (running_) {
    int64_t ts = get_millis();
    LOG(INFO) << "begin refresh tablets status...";
    sysinfo_.RefreshTabletsStatus(tablet_manager_.get());

    LOG(INFO) << "finish refresh tablets status. cost: " << get_millis() - ts
              << " ms, next round after " << FLAGS_tera_refresh_tablets_status_interval_ms << " ms";
    exit_event_.TimeWait(FLAGS_tera_refresh_tablets_status_interval_ms);
  }
  LOG(INFO) << "exit refresh tablets status";
}

void TabletNodeImpl::UnloadTabletProc(io::TabletIO* tablet_io, Counter* worker_count) {
  LOG(INFO) << "begin to unload tablet: " << *tablet_io;
  StatusCode status;
  if (!tablet_io->Unload(&status)) {
    LOG(ERROR) << "fail to unload tablet: " << *tablet_io
               << ", status: " << StatusCodeToString(status);
  } else {
    LOG(INFO) << "unload tablet success: " << *tablet_io;
  }
  tablet_io->DecRef();
  worker_count->Dec();
}

StatusCode TabletNodeImpl::QueryTabletStatus(const std::string& table_name,
                                             const std::string& key_start,
                                             const std::string& key_end) {
  StatusCode status;
  io::TabletIO* tablet_io = tablet_manager_->GetTablet(table_name, key_start, key_end, &status);
  if (tablet_io == NULL) {
    VLOG(15) << "fail to get tablet: " << table_name << " [" << DebugString(key_start) << ", "
             << DebugString(key_end) << "], status: " << StatusCodeToString(status);
    return kKeyNotInRange;
  }
  return static_cast<StatusCode>(tablet_io->GetStatus());
}

void TabletNodeImpl::LoadTablet(const LoadTabletRequest* request, LoadTabletResponse* response) {
  response->set_sequence_id(request->sequence_id());
  std::string sid = GetSessionId();
  if (!request->has_session_id() || (sid.size() == 0) ||
      request->session_id().compare(0, sid.size(), sid) != 0) {
    LOG(WARNING) << "load session id not match: tablet " << request->path() << ", session_id "
                 << request->session_id() << ", ts_id " << sid;
    response->set_status(kIllegalAccess);
    return;
  }
  if (request->schema().locality_groups_size() < 1) {
    LOG(WARNING) << "No localitygroups in schema: " << request->tablet_name();
    response->set_status(kIllegalAccess);
    return;
  }

  const std::string& key_start = request->key_range().key_start();
  const std::string& key_end = request->key_range().key_end();
  const TableSchema& schema = request->schema();
  int64_t create_time = request->create_time();
  uint64_t version = request->version();

  std::vector<uint64_t> parent_tablets;
  for (int i = 0; i < request->parent_tablets_size(); ++i) {
    CHECK(i < 2) << "parent_tablets should less than 2: " << i;
    parent_tablets.push_back(request->parent_tablets(i));
  }
  std::set<std::string> ignore_err_lgs;
  for (int i = 0; i < request->ignore_err_lgs_size(); ++i) {
    VLOG(10) << "oops lg:" << request->ignore_err_lgs(i);
    ignore_err_lgs.insert(request->ignore_err_lgs(i));
  }

  io::TabletIO* tablet_io = NULL;
  StatusCode status = kTabletNodeOk;
  if (!tablet_manager_->AddTablet(request->tablet_name(), request->path(), key_start, key_end,
                                  create_time, version, &tablet_io, &status)) {
    io::TabletIO::TabletStatus tablet_status = tablet_io->GetStatus();
    if (tablet_status == io::TabletIO::TabletStatus::kOnLoad ||
        tablet_status == io::TabletIO::TabletStatus::kReady) {
      VLOG(6) << "ignore this load tablet request, tablet: " << request->path() << " ["
              << DebugString(key_start) << ", " << DebugString(key_end)
              << "], status: " << StatusCodeToString((StatusCode)tablet_status);
    } else {
      LOG(ERROR) << "fail to add tablet: " << request->path() << " [" << DebugString(key_start)
                 << ", " << DebugString(key_end)
                 << "], status: " << StatusCodeToString((StatusCode)tablet_status);
    }
    response->set_status((StatusCode)tablet_status);
    tablet_io->DecRef();
  } else {
    LOG(INFO) << "start load tablet, id: " << request->sequence_id() << ", sessionid "
              << request->session_id() << ", ts_id " << sid << ", table: " << request->tablet_name()
              << ", range: [" << DebugString(key_start) << ", " << DebugString(key_end)
              << "], path: " << request->path() << ", ctimestamp(us): " << create_time
              << ", version: " << version << ", parent: "
              << (request->parent_tablets_size() > 0 ? request->parent_tablets(0) : 0)
              << ", schema: " << request->schema().ShortDebugString();
    /// TODO: User per user memery_cache according to user quota.
    tablet_io->SetMemoryCache(m_memory_cache);
    if (!tablet_io->Load(schema, request->path(), parent_tablets, ignore_err_lgs, ldb_logger_,
                         ldb_block_cache_, ldb_table_cache_, &status)) {
      std::string err_msg = tablet_io->GetLastErrorMessage();
      tablet_io->DecRef();
      LOG(ERROR) << "fail to load tablet: " << request->path() << " [" << DebugString(key_start)
                 << ", " << DebugString(key_end) << "], status: " << StatusCodeToString(status)
                 << ",err_msg: " << err_msg;
      if (!tablet_manager_->RemoveTablet(request->tablet_name(), key_start, key_end, &status)) {
        LOG(ERROR) << "fail to remove tablet: " << request->path() << " [" << DebugString(key_start)
                   << ", " << DebugString(key_end) << "], status: " << StatusCodeToString(status);
      }
      response->set_status(kIOError);
      std::string load_context = tera::sdk::StatTable::SerializeLoadContext(*request, sid);
      std::string msg = tera::sdk::StatTable::SerializeCorrupt(
          sdk::CorruptPhase::kLoading, local_addr_, request->path(), load_context, err_msg);
      response->set_detail_fail_msg(msg);
    } else {
      tablet_io->DecRef();
      response->set_status(kTabletNodeOk);
    }
  }

  LOG(INFO) << "load tablet: " << request->path() << " [" << DebugString(key_start) << ", "
            << DebugString(key_end) << "]";
}

bool TabletNodeImpl::UnloadTablet(const std::string& tablet_name, const std::string& start,
                                  const std::string& end, StatusCode* status) {
  io::TabletIO* tablet_io = tablet_manager_->GetTablet(tablet_name, start, end, status);
  if (tablet_io == NULL) {
    LOG(WARNING) << "unload fail to get tablet: " << tablet_name << " [" << DebugString(start)
                 << ", " << DebugString(end) << "], status: " << StatusCodeToString(*status);
    *status = kKeyNotInRange;
    return false;
  }

  if (!tablet_io->Unload(status)) {
    io::TabletIO::TabletStatus tablet_status = tablet_io->GetStatus();
    if (tablet_status == io::TabletIO::TabletStatus::kUnloading ||
        tablet_status == io::TabletIO::TabletStatus::kUnloading2) {
      VLOG(6) << "ignore this unload tablet request: " << tablet_io->GetTablePath() << "["
              << DebugString(start) << "," << DebugString(end)
              << "], status: " << StatusCodeToString((StatusCode)tablet_status);
    } else {
      LOG(ERROR) << "fail to unload tablet: " << tablet_io->GetTablePath() << " ["
                 << DebugString(start) << ", " << DebugString(end)
                 << "], status: " << StatusCodeToString(*status);
    }
    *status = (StatusCode)tablet_status;
    tablet_io->DecRef();
    return false;
  }
  LOG(INFO) << "unload tablet: " << tablet_io->GetTablePath() << " [" << DebugString(start) << ", "
            << DebugString(end) << "]";
  tablet_io->DecRef();

  if (!tablet_manager_->RemoveTablet(tablet_name, start, end, status)) {
    LOG(ERROR) << "fail to remove tablet: " << tablet_name << " [" << DebugString(start) << ", "
               << DebugString(end) << "], status: " << StatusCodeToString(*status);
  }
  *status = kTabletNodeOk;
  return true;
}

void TabletNodeImpl::UnloadTablet(const UnloadTabletRequest* request,
                                  UnloadTabletResponse* response) {
  response->set_sequence_id(request->sequence_id());
  std::string sid = GetSessionId();
  // master vervison lower than 2.10 has not session_id field, so just
  if (request->has_session_id() &&
      ((sid.size() == 0) || request->session_id().compare(0, sid.size(), sid) != 0)) {
    LOG(WARNING) << "unload session id not match, seq_id: " << request->sequence_id()
                 << "tablet: " << request->tablet_name() << ", ["
                 << request->key_range().key_start() << ", " << request->key_range().key_end()
                 << "], session_id " << request->session_id() << ", ts_id " << sid;
    response->set_status(kIllegalAccess);
    return;
  }

  StatusCode status = kTabletNodeOk;
  UnloadTablet(request->tablet_name(), request->key_range().key_start(),
               request->key_range().key_end(), &status);
  response->set_status(status);
}

void TabletNodeImpl::CompactTablet(const CompactTabletRequest* request,
                                   CompactTabletResponse* response,
                                   google::protobuf::Closure* done) {
  response->set_sequence_id(request->sequence_id());
  StatusCode status = kTabletNodeOk;
  io::TabletIO* tablet_io =
      tablet_manager_->GetTablet(request->tablet_name(), request->key_range().key_start(),
                                 request->key_range().key_end(), &status);
  if (tablet_io == NULL) {
    LOG(WARNING) << "compact fail to get tablet: " << request->tablet_name() << " ["
                 << DebugString(request->key_range().key_start()) << ", "
                 << DebugString(request->key_range().key_end())
                 << "], status: " << StatusCodeToString(status);
    response->set_status(kKeyNotInRange);
    done->Run();
    return;
  }
  LOG(INFO) << "start compact tablet: " << tablet_io->GetTablePath() << " ["
            << DebugString(tablet_io->GetStartKey()) << ", " << DebugString(tablet_io->GetEndKey())
            << "]";

  if (request->has_lg_no() && request->lg_no() >= 0) {
    tablet_io->Compact(request->lg_no(), &status);
  } else {
    tablet_io->Compact(-1, &status);
  }
  CompactStatus compact_status = tablet_io->GetCompactStatus();
  response->set_status(status);
  response->set_compact_status(compact_status);
  uint64_t compact_size = 0;
  tablet_io->GetDataSize(&compact_size);
  response->set_compact_size(compact_size);
  LOG(INFO) << "compact tablet: " << tablet_io->GetTablePath() << " ["
            << DebugString(tablet_io->GetStartKey()) << ", " << DebugString(tablet_io->GetEndKey())
            << "], status: " << StatusCodeToString(status) << ", compacted size: " << compact_size;
  tablet_io->DecRef();
  done->Run();
}

void TabletNodeImpl::Update(const UpdateRequest* request, UpdateResponse* response,
                            google::protobuf::Closure* done) {
  response->set_sequence_id(request->sequence_id());
  switch (request->type()) {
    case kUpdateSchema:
      LOG(INFO) << "[update] new schema:" << request->schema().DebugString();
      if (ApplySchema(request)) {
        LOG(INFO) << "[update] ok";
        response->set_status(kTabletNodeOk);
      } else {
        LOG(INFO) << "[update] failed";
        response->set_status(kInvalidArgument);
      }
      done->Run();
      break;
    default:
      LOG(INFO) << "[update] unknown cmd";
      response->set_status(kInvalidArgument);
      done->Run();
      break;
  }
}

void TabletNodeImpl::WriteTablet(const WriteTabletRequest* request, WriteTabletResponse* response,
                                 google::protobuf::Closure* done, WriteRpcTimer* timer) {
  response->set_sequence_id(request->sequence_id());
  StatusCode status = kTabletNodeOk;

  std::map<io::TabletIO*, WriteTabletTask*> tablet_task_map;
  std::map<io::TabletIO*, WriteTabletTask*>::iterator it;

  int32_t row_num = request->row_list_size();
  if (row_num == 0) {
    response->set_status(kTabletNodeOk);
    done->Run();
    if (NULL != timer) {
      RpcTimerList::Instance()->Erase(timer);
      delete timer;
    }
    return;
  }

  std::shared_ptr<Counter> row_done_counter(new Counter);
  for (int32_t i = 0; i < row_num; i++) {
    io::TabletIO* tablet_io =
        tablet_manager_->GetTablet(request->tablet_name(), request->row_list(i).row_key(), &status);
    if (tablet_io == NULL) {
      write_range_error_counter.Inc();
    }
    it = tablet_task_map.find(tablet_io);
    WriteTabletTask* tablet_task = NULL;
    if (it == tablet_task_map.end()) {
      // keep one ref to tablet_io
      tablet_task = tablet_task_map[tablet_io] =
          new WriteTabletTask(request, response, done, timer, row_done_counter);
    } else {
      if (tablet_io != NULL) {
        tablet_io->DecRef();
      }
      tablet_task = it->second;
    }
    tablet_task->row_mutation_vec.push_back(&request->row_list(i));
    tablet_task->row_status_vec.push_back(kTabletNodeOk);
    tablet_task->row_index_vec.push_back(i);
  }

  // reserve response status list space
  response->set_status(kTabletNodeOk);
  response->mutable_row_status_list()->Reserve(row_num);
  for (int32_t i = 0; i < row_num; i++) {
    response->mutable_row_status_list()->AddAlreadyReserved();
  }

  for (it = tablet_task_map.begin(); it != tablet_task_map.end(); ++it) {
    io::TabletIO* tablet_io = it->first;
    WriteTabletTask* tablet_task = it->second;
    if (tablet_io == NULL) {
      WriteTabletFail(tablet_task, kKeyNotInRange);
    } else if (!tablet_io->Write(
                   &tablet_task->row_mutation_vec, &tablet_task->row_status_vec,
                   request->is_instant(),
                   std::bind(&TabletNodeImpl::WriteTabletCallback, this, tablet_task, _1, _2),
                   &status)) {
      tablet_io->DecRef();
      WriteTabletFail(tablet_task, status);
    } else {
      tablet_io->DecRef();
    }
  }
}

void TabletNodeImpl::WriteTabletFail(WriteTabletTask* tablet_task, StatusCode status) {
  int32_t row_num = tablet_task->row_status_vec.size();
  if (status != kKeyNotInRange) {
    write_error_counter.Add(row_num);
  }
  for (int32_t i = 0; i < row_num; i++) {
    tablet_task->row_status_vec[i] = status;
  }
  WriteTabletCallback(tablet_task, &tablet_task->row_mutation_vec, &tablet_task->row_status_vec);
}

void TabletNodeImpl::WriteTabletCallback(WriteTabletTask* tablet_task,
                                         std::vector<const RowMutationSequence*>* row_mutation_vec,
                                         std::vector<StatusCode>* status_vec) {
  int32_t index_num = tablet_task->row_index_vec.size();
  for (int32_t i = 0; i < index_num; i++) {
    int32_t index = tablet_task->row_index_vec[i];
    tablet_task->response->mutable_row_status_list()->Set(index, (*status_vec)[i]);
  }

  if (tablet_task->row_done_counter->Add(index_num) == tablet_task->request->row_list_size()) {
    tablet_task->done->Run();
    if (NULL != tablet_task->timer) {
      RpcTimerList::Instance()->Erase(tablet_task->timer);
      delete tablet_task->timer;
    }
  }

  delete tablet_task;
}

void TabletNodeImpl::CmdCtrl(const TsCmdCtrlRequest* request, TsCmdCtrlResponse* response,
                             google::protobuf::Closure* done) {
  response->set_sequence_id(request->sequence_id());
  if (request->command() == "reload config") {
    if (utils::LoadFlagFile(FLAGS_flagfile)) {
      LOG(INFO) << "[reload config] done";
      response->set_status(kTabletNodeOk);
    } else {
      LOG(ERROR) << "[reload config] config file not found";
      response->set_status(kInvalidArgument);
    }
  } else {
    response->set_status(kInvalidArgument);
  }
  done->Run();
}

bool TabletNodeImpl::ApplySchema(const UpdateRequest* request) {
  StatusCode status;
  io::TabletIO* tablet_io =
      tablet_manager_->GetTablet(request->tablet_name(), request->key_range().key_start(),
                                 request->key_range().key_end(), &status);
  if (tablet_io == NULL) {
    LOG(INFO) << "[update] tablet not found";
    return false;
  }
  tablet_io->ApplySchema(request->schema());
  tablet_io->DecRef();
  return true;
}

void TabletNodeImpl::Query(const QueryRequest* request, QueryResponse* response,
                           google::protobuf::Closure* done) {
  response->set_sequence_id(request->sequence_id());
  response->set_status(kTabletNodeOk);

  TabletNodeInfo* ts_info = response->mutable_tabletnode_info();
  sysinfo_.GetTabletNodeInfo(ts_info);
  TabletMetaList* meta_list = response->mutable_tabletmeta_list();
  sysinfo_.GetTabletMetaList(meta_list);

  if (request->has_is_gc_query() && request->is_gc_query()) {
    std::vector<TabletInheritedFileInfo> inh_infos;
    GetInheritedLiveFiles(&inh_infos);
    for (size_t i = 0; i < inh_infos.size(); i++) {
      TabletInheritedFileInfo* inh_info = response->add_tablet_inh_file_infos();
      inh_info->CopyFrom(inh_infos[i]);
    }

    // only for compatible with old master
    std::vector<InheritedLiveFiles> inherited;
    GetInheritedLiveFiles(inherited);
    for (size_t i = 0; i < inherited.size(); ++i) {
      InheritedLiveFiles* files = response->add_inh_live_files();
      *files = inherited[i];
    }
  }

  // if have background errors, package into 'response' and return to 'master'
  std::vector<TabletBackgroundErrorInfo> background_errors;
  GetBackgroundErrors(&background_errors);
  for (auto background_error : background_errors) {
    TabletBackgroundErrorInfo* tablet_background_error = response->add_tablet_background_errors();
    tablet_background_error->CopyFrom(background_error);
  }
  done->Run();
}

void TabletNodeImpl::RefreshAndDumpSysInfo() {
  int64_t cur_ts = get_micros();

  sysinfo_.CollectTabletNodeInfo(tablet_manager_.get(), local_addr_);
  sysinfo_.CollectHardwareInfo();
  sysinfo_.SetTimeStamp(cur_ts);
  sysinfo_.UpdateWriteFlowController();
  sysinfo_.DumpLog();

  VLOG(15) << "collect sysinfo finished, time used: " << get_micros() - cur_ts << " us.";
}

void TabletNodeImpl::ScanTablet(const ScanTabletRequest* request, ScanTabletResponse* response,
                                google::protobuf::Closure* done) {
  const int64_t PACK_MAX_SIZE = static_cast<int64_t>(FLAGS_tera_tabletnode_scan_pack_max_size)
                                << 10;
  // const std::string& start_key = request->key_range().key_start();
  // const std::string& end_key = request->key_range().key_end();
  int64_t buffer_limit = request->buffer_limit();
  if (buffer_limit > PACK_MAX_SIZE) {
    buffer_limit = PACK_MAX_SIZE;
  }
  // VLOG(5) << "ScanTablet() start=[" << start_key
  //    << "], end=[" << end_key << "]";
  if (request->has_sequence_id()) {
    response->set_sequence_id(request->sequence_id());
  }

  StatusCode status = kTabletNodeOk;
  io::TabletIO* tablet_io = NULL;
  tablet_io = tablet_manager_->GetTablet(request->table_name(), request->start(), &status);

  if (tablet_io == NULL) {
    scan_range_error_counter.Inc();
    response->set_status(status);
    done->Run();
  } else {
    response->set_end(tablet_io->GetEndKey());
    if (!tablet_io->ScanRows(request, response, done)) {
      scan_error_counter.Inc();
    }
    tablet_io->DecRef();
  }
}

void TabletNodeImpl::ComputeSplitKey(const SplitTabletRequest* request,
                                     SplitTabletResponse* response,
                                     google::protobuf::Closure* done) {
  response->set_sequence_id(request->sequence_id());

  std::string split_key;
  StatusCode status = kTabletNodeOk;
  io::TabletIO* tablet_io =
      tablet_manager_->GetTablet(request->tablet_name(), request->key_range().key_start(),
                                 request->key_range().key_end(), &status);
  if (tablet_io == NULL) {
    LOG(WARNING) << "split fail to get tablet: " << request->tablet_name() << " ["
                 << DebugString(request->key_range().key_start()) << ", "
                 << DebugString(request->key_range().key_end())
                 << "], status: " << StatusCodeToString(status);
    response->set_status(kKeyNotInRange);
    done->Run();
    return;
  }

  if (!tablet_io->Split(&split_key, &status)) {
    LOG(ERROR) << "fail to split tablet: " << tablet_io->GetTablePath() << " ["
               << DebugString(tablet_io->GetStartKey()) << ", "
               << DebugString(tablet_io->GetEndKey()) << "], split_key: " << DebugString(split_key)
               << ". status: " << StatusCodeToString(status);
    if (status == kTableNotSupport) {
      response->set_status(kTableNotSupport);
    } else {
      response->set_status((StatusCode)tablet_io->GetStatus());
    }
    tablet_io->DecRef();
    done->Run();
    return;
  }
  LOG(INFO) << "split tablet: " << tablet_io->GetTablePath() << " ["
            << DebugString(tablet_io->GetStartKey()) << ", " << DebugString(tablet_io->GetEndKey())
            << "], split key: " << DebugString(split_key);
  response->set_status(kTabletNodeOk);
  response->add_split_keys(split_key);
  tablet_io->DecRef();
  done->Run();
}

bool TabletNodeImpl::CheckInKeyRange(const KeyList& key_list, const std::string& key_start,
                                     const std::string& key_end) {
  for (int32_t i = 0; i < key_list.size(); ++i) {
    const std::string& key = key_list.Get(i);
    if (key < key_start || (key_end != "" && key >= key_end)) {
      return false;
    }
  }
  return true;
}

bool TabletNodeImpl::CheckInKeyRange(const KeyValueList& pair_list, const std::string& key_start,
                                     const std::string& key_end) {
  for (int32_t i = 0; i < pair_list.size(); ++i) {
    const std::string& key = pair_list.Get(i).key();
    if (key < key_start || (key_end != "" && key >= key_end)) {
      return false;
    }
  }
  return true;
}

bool TabletNodeImpl::CheckInKeyRange(const RowReaderList& reader_list, const std::string& key_start,
                                     const std::string& key_end) {
  for (int32_t i = 0; i < reader_list.size(); ++i) {
    const std::string& key = reader_list.Get(i).key();
    if (key < key_start || (key_end != "" && key >= key_end)) {
      return false;
    }
  }
  return true;
}

bool TabletNodeImpl::CheckInKeyRange(const RowMutationList& row_list, const std::string& key_start,
                                     const std::string& key_end) {
  for (int32_t i = 0; i < row_list.size(); ++i) {
    const std::string& key = row_list.Get(i).row_key();
    if (key < key_start || (key_end != "" && key >= key_end)) {
      return false;
    }
  }
  return true;
}

/////////// common ////////////

void TabletNodeImpl::EnterSafeMode() { SetTabletNodeStatus(kIsReadonly); }

void TabletNodeImpl::LeaveSafeMode() { SetTabletNodeStatus(kIsRunning); }

void TabletNodeImpl::ExitService() {
  LOG(FATAL) << "master kick me!";
  _exit(1);
}

void TabletNodeImpl::SetTabletNodeStatus(const TabletNodeStatus& status) {
  MutexLock lock(&status_mutex_);
  status_ = status;
}

TabletNodeImpl::TabletNodeStatus TabletNodeImpl::GetTabletNodeStatus() {
  MutexLock lock(&status_mutex_);
  return status_;
}

void TabletNodeImpl::SetRootTabletAddr(const std::string& root_tablet_addr) {
  root_tablet_addr_ = root_tablet_addr;
}

/*
 * all cached tablets/files:
 * ------------------------------------------
 * | active tablets  |   inactive tablets   |
 * |                 |                      |
 * |                 |    all    |    to    |
 * |                 | inherited | *DELETE* |
 * |                 |    files  |          |
 * ------------------------------------------
 */
void TabletNodeImpl::GarbageCollect() {
  int64_t start_ms = get_micros();
  LOG(INFO) << "[gc] start...";

  // get all inherited sst files
  std::vector<InheritedLiveFiles> table_files;
  GetInheritedLiveFiles(table_files);
  std::set<std::string> inherited_files;
  for (size_t t = 0; t < table_files.size(); ++t) {
    const InheritedLiveFiles& live = table_files[t];
    int lg_num = live.lg_live_files_size();
    for (int lg = 0; lg < lg_num; ++lg) {
      const LgInheritedLiveFiles& lg_live_files = live.lg_live_files(lg);
      for (int f = 0; f < lg_live_files.file_number_size(); ++f) {
        std::string file_path =
            leveldb::BuildTableFilePath(live.table_name(), lg, lg_live_files.file_number(f));
        inherited_files.insert(file_path);
        // file_path : table-name/tablet-xxx/lg-num/xxx.sst
        VLOG(GC_LOG_LEVEL) << "[gc] inherited live file: " << file_path;
      }
    }
  }

  // get all active tablets
  std::vector<TabletMeta*> tablet_meta_list;
  std::set<std::string> active_tablets;
  tablet_manager_->GetAllTabletMeta(&tablet_meta_list);
  std::vector<TabletMeta*>::iterator it = tablet_meta_list.begin();
  for (; it != tablet_meta_list.end(); ++it) {
    VLOG(GC_LOG_LEVEL) << "[gc] Active Tablet: " << (*it)->path();
    active_tablets.insert((*it)->path());
    delete (*it);
  }

  // collect persistent cache garbage
  PersistentCacheGarbageCollect(inherited_files, active_tablets);

  // collect flash directories
  leveldb::FlashEnv* flash_env = (leveldb::FlashEnv*)io::LeveldbFlashEnv();
  if (flash_env) {
    const std::vector<std::string>& flash_paths = flash_env->GetFlashPaths();
    for (size_t d = 0; d < flash_paths.size(); ++d) {
      std::string flash_dir = flash_paths[d] + FLAGS_tera_tabletnode_path_prefix;
      GarbageCollectInPath(flash_dir, leveldb::Env::Default(), inherited_files, active_tablets);
    }
  }

  // collect memory env
  leveldb::Env* mem_env = io::LeveldbMemEnv()->CacheEnv();
  GarbageCollectInPath(FLAGS_tera_tabletnode_path_prefix, mem_env, inherited_files, active_tablets);

  LOG(INFO) << "[gc] finished, time used: " << get_micros() - start_ms << " us.";
}

void TabletNodeImpl::GarbageCollectInPath(const std::string& path, leveldb::Env* env,
                                          const std::set<std::string>& inherited_files,
                                          const std::set<std::string>& active_tablets) {
  std::vector<std::string> table_dirs;
  env->GetChildren(path, &table_dirs);
  for (size_t i = 0; i < table_dirs.size(); ++i) {
    std::vector<std::string> cached_tablets;
    env->GetChildren(path + "/" + table_dirs[i], &cached_tablets);
    if (cached_tablets.size() == 0) {
      VLOG(GC_LOG_LEVEL) << "[gc] this directory is empty, delete it: "
                         << path + "/" + table_dirs[i];
      env->DeleteDir(path + "/" + table_dirs[i]);
      continue;
    }
    for (size_t j = 0; j < cached_tablets.size(); ++j) {
      std::string tablet_dir = table_dirs[i] + "/" + cached_tablets[j];
      VLOG(GC_LOG_LEVEL) << "[gc] Cached Tablet: " << tablet_dir;
      if (active_tablets.find(tablet_dir) != active_tablets.end()) {
        // active tablets
        continue;
      }
      std::string inactive_tablet_dir = path + "/" + tablet_dir;
      VLOG(GC_LOG_LEVEL) << "[gc] inactive_tablet directory:" << inactive_tablet_dir;
      std::vector<std::string> lgs;
      env->GetChildren(inactive_tablet_dir, &lgs);
      if (lgs.size() == 0) {
        VLOG(GC_LOG_LEVEL) << "[gc] this directory is empty, delete it: " << inactive_tablet_dir;
        env->DeleteDir(inactive_tablet_dir);
        continue;
      }
      for (size_t lg = 0; lg < lgs.size(); ++lg) {
        std::vector<std::string> files;
        env->GetChildren(inactive_tablet_dir + "/" + lgs[lg], &files);
        if (files.size() == 0) {
          VLOG(GC_LOG_LEVEL) << "[gc] this directory is empty, delete it: "
                             << inactive_tablet_dir + "/" + lgs[lg];
          env->DeleteDir(inactive_tablet_dir + "/" + lgs[lg]);
          continue;
        }
        for (size_t f = 0; f < files.size(); ++f) {
          std::string file = files[f];
          std::string pathname = inactive_tablet_dir + "/" + lgs[lg] + "/" + file;
          if (inherited_files.find(tablet_dir + "/" + lgs[lg] + "/" + file) ==
              inherited_files.end()) {
            VLOG(GC_LOG_LEVEL) << "[gc] delete sst file: " << pathname;
            env->DeleteFile(pathname);

          } else {
            VLOG(GC_LOG_LEVEL) << "[gc] skip inherited file: " << pathname;
          }
        }  // sst file
      }    // lg
    }      // tablet
  }        // table
}

void TabletNodeImpl::SetSessionId(const std::string& session_id) {
  MutexLock lock(&status_mutex_);
  session_id_ = session_id;
}

std::string TabletNodeImpl::GetSessionId() {
  MutexLock lock(&status_mutex_);
  return session_id_;
}

TabletNodeSysInfo& TabletNodeImpl::GetSysInfo() { return sysinfo_; }

void TabletNodeImpl::TryReleaseMallocCache() {
  LOG(INFO) << "TryReleaseMallocCache()";
  size_t free_heap_bytes = 0;
  MallocExtension::instance()->GetNumericProperty("tcmalloc.pageheap_free_bytes", &free_heap_bytes);
  if (free_heap_bytes == 0) {
    return;
  }

  VLOG(5) << "tcmalloc cache size: " << free_heap_bytes;

  if (free_heap_bytes < 10 * 1024 * 1024) {
    MallocExtension::instance()->ReleaseFreeMemory();
    VLOG(5) << "release tcmalloc cache size: " << free_heap_bytes;
  } else {
    // have workload
    MallocExtension::instance()->ReleaseToSystem(free_heap_bytes / 2);
    VLOG(5) << "release tcmalloc cache size: " << free_heap_bytes / 2;
  }
}

void TabletNodeImpl::ReleaseMallocCache() {
  MutexLock locker(&mutex_);

  TryReleaseMallocCache();

  release_cache_timer_id_ = kInvalidTimerId;
  EnableReleaseMallocCacheTimer();
}

void TabletNodeImpl::EnableReleaseMallocCacheTimer(int32_t expand_factor) {
  assert(release_cache_timer_id_ == kInvalidTimerId);
  ThreadPool::Task task = std::bind(&TabletNodeImpl::ReleaseMallocCache, this);
  int64_t timeout_period = expand_factor * 1000LL * FLAGS_tera_tabletnode_tcm_cache_release_period;
  release_cache_timer_id_ = thread_pool_->DelayTask(timeout_period, task);
}

void TabletNodeImpl::DisableReleaseMallocCacheTimer() {
  if (release_cache_timer_id_ != kInvalidTimerId) {
    thread_pool_->CancelTask(release_cache_timer_id_);
    release_cache_timer_id_ = kInvalidTimerId;
  }
}

void TabletNodeImpl::GetInheritedLiveFiles(std::vector<TabletInheritedFileInfo>* inherited) {
  std::vector<io::TabletIO*> tablet_ios;
  tablet_manager_->GetAllTablets(&tablet_ios);
  for (size_t tablet_id = 0; tablet_id < tablet_ios.size(); tablet_id++) {
    io::TabletIO* tablet_io = tablet_ios[tablet_id];
    std::vector<std::set<uint64_t> > tablet_files;
    if (tablet_io->AddInheritedLiveFiles(&tablet_files)) {
      TabletInheritedFileInfo inh_file_info;
      inh_file_info.set_table_name(tablet_io->GetTableName());
      inh_file_info.set_key_start(tablet_io->GetStartKey());
      inh_file_info.set_key_end(tablet_io->GetEndKey());
      for (size_t lg_id = 0; lg_id < tablet_files.size(); lg_id++) {
        VLOG(10) << "[gc] " << tablet_io->GetTablePath() << " add inherited file, lg " << lg_id
                 << ", " << tablet_files[lg_id].size() << " files total";
        LgInheritedLiveFiles* lg_files = inh_file_info.add_lg_inh_files();
        lg_files->set_lg_no(lg_id);
        std::set<uint64_t>::iterator file_it = tablet_files[lg_id].begin();
        for (; file_it != tablet_files[lg_id].end(); ++file_it) {
          lg_files->add_file_number(*file_it);
        }
      }
      inherited->push_back(inh_file_info);
    }
    tablet_io->DecRef();
  }
}

void TabletNodeImpl::GetInheritedLiveFiles(std::vector<InheritedLiveFiles>& inherited) {
  std::set<std::string> not_ready_tables;
  typedef std::vector<std::set<uint64_t> > TableSet;
  std::map<std::string, TableSet> live;

  std::vector<io::TabletIO*> tablet_ios;
  tablet_manager_->GetAllTablets(&tablet_ios);
  std::vector<io::TabletIO*>::iterator it = tablet_ios.begin();
  for (; it != tablet_ios.end(); ++it) {
    io::TabletIO* tablet_io = *it;
    const std::string& tablename = tablet_io->GetTableName();
    if (not_ready_tables.find(tablename) == not_ready_tables.end() &&
        !tablet_io->AddInheritedLiveFiles(&live[tablename])) {
      VLOG(10) << "[gc] " << tablet_io->GetTablePath() << " is not ready, skip it.";
      not_ready_tables.insert(tablename);
      live[tablename].clear();
    }
    tablet_io->DecRef();
  }

  int total = 0;
  std::map<std::string, TableSet>::iterator live_it = live.begin();
  for (; live_it != live.end(); ++live_it) {
    VLOG(10) << "[gc] add inherited file, table " << live_it->first;
    if (not_ready_tables.find(live_it->first) != not_ready_tables.end()) {
      VLOG(10) << "[gc] table: " << live_it->first << " is not ready, skip it.";
      continue;
    }
    InheritedLiveFiles table;
    table.set_table_name(live_it->first);
    for (size_t i = 0; i < live_it->second.size(); ++i) {
      VLOG(10) << "[gc] add inherited file, lg " << i << ", " << (live_it->second)[i].size()
               << " files total";
      LgInheritedLiveFiles* lg_files = table.add_lg_live_files();
      lg_files->set_lg_no(i);
      std::set<uint64_t>::iterator file_it = (live_it->second)[i].begin();
      for (; file_it != (live_it->second)[i].end(); ++file_it) {
        lg_files->add_file_number(*file_it);
        total++;
      }
    }
    inherited.push_back(table);
  }
  LOG(INFO) << "[gc] add inherited file " << total << " total";
}

void TabletNodeImpl::GetBackgroundErrors(
    std::vector<TabletBackgroundErrorInfo>* background_errors) {
  std::vector<io::TabletIO*> tablet_ios;
  tablet_manager_->GetAllTablets(&tablet_ios);
  std::vector<io::TabletIO*>::iterator it = tablet_ios.begin();
  uint64_t reported_error_msg_len = 0;
  while (it != tablet_ios.end()) {
    io::TabletIO* tablet_io = *it;
    if (tablet_io->ShouldForceUnloadOnError()) {
      LOG(WARNING) << *tablet_io << ", has internal error triggered unload";
      StatusCode status;
      if (!tablet_io->Unload(&status)) {
        LOG(ERROR) << *tablet_io
                   << ", Unload tablet failed, status: " << StatusCodeToString(status);
      }
      if (!tablet_manager_->RemoveTablet(tablet_io->GetTableName(), tablet_io->GetStartKey(),
                                         tablet_io->GetEndKey(), &status)) {
        LOG(ERROR) << *tablet_io
                   << ", remove from TabletManager failed, status: " << StatusCodeToString(status);
      }
      tablet_io->DecRef();
      it = tablet_ios.erase(it);
      continue;
    }
    std::string background_error_msg = "";
    tablet_io->CheckBackgroundError(&background_error_msg);
    if (!background_error_msg.empty()) {
      std::string msg = tera::sdk::StatTable::SerializeCorrupt(
          sdk::CorruptPhase::kCompacting, local_addr_, tablet_io->GetTablePath(), "",
          background_error_msg);

      VLOG(15) << "background error @ " << tablet_io->GetTablePath() << ":" << background_error_msg;
      reported_error_msg_len += msg.length();

      // if the length of error message overrun the limit，
      // only part of them would be reported
      if (reported_error_msg_len < kReportErrorSize) {
        tera::TabletBackgroundErrorInfo background_error;
        background_error.set_tablet_name(tablet_io->GetTablePath());
        background_error.set_detail_info(msg);
        background_errors->push_back(background_error);
      }
    }
    ++it;
    tablet_io->DecRef();
  }
}

void TabletNodeImpl::RefreshLevelSize() {
  std::vector<io::TabletIO*> tablet_ios;
  tablet_manager_->GetAllTablets(&tablet_ios);
  std::vector<io::TabletIO*>::iterator it = tablet_ios.begin();
  std::vector<int64_t> level_size_total(leveldb::config::kNumLevels, 0);
  std::vector<int64_t> db_level_size;
  while (it != tablet_ios.end()) {
    io::TabletIO* tablet_io = *it;
    if (tablet_io->ShouldForceUnloadOnError()) {
      LOG(WARNING) << *tablet_io << ", has internal error triggered unload";
      StatusCode status;
      if (!tablet_io->Unload(&status)) {
        LOG(ERROR) << *tablet_io
                   << ", Unload tablet failed, status: " << StatusCodeToString(status);
      }
      if (!tablet_manager_->RemoveTablet(tablet_io->GetTableName(), tablet_io->GetStartKey(),
                                         tablet_io->GetEndKey(), &status)) {
        LOG(ERROR) << *tablet_io
                   << ", remove from TabletManager failed, status: " << StatusCodeToString(status);
      }
      tablet_io->DecRef();
      it = tablet_ios.erase(it);
      continue;
    }
    if (tablet_io->GetDBLevelSize(&db_level_size)) {
      assert(db_level_size.size() == level_size_total.size());
      for (int level = 0; level != leveldb::config::kNumLevels; ++level) {
        level_size_total[level] += db_level_size[level];
      }
    }
    tablet_io->DecRef();
    it = tablet_ios.erase(it);
  }
  for (int level = 0; level != leveldb::config::kNumLevels; ++level) {
    level_size_[level].Set(level_size_total[level]);
  }
}

ReadTabletTask::ReadTabletTask(int64_t start_micros, std::shared_ptr<TabletManager> tablet_manager,
                               const ReadTabletRequest* request, ReadTabletResponse* response,
                               google::protobuf::Closure* done, ThreadPool* read_thread_pool)
    : tablet_manager_(tablet_manager),
      request_(request),
      response_(response),
      done_(done),
      read_thread_pool_(read_thread_pool) {
  total_row_num_ = request_->row_info_list_size();
  snapshot_id_ = request_->snapshot_id() == 0 ? 0 : request_->snapshot_id();
  response->set_sequence_id(request->sequence_id());

  int64_t client_timeout_ms = std::numeric_limits<int64_t>::max() / 2;
  if (request_->has_client_timeout_ms()) {
    client_timeout_ms = request->client_timeout_ms();
  }
  end_time_ms_ = start_micros / 1000 + client_timeout_ms;
  VLOG(20) << "start_ms: " << start_micros / 1000 << ", client_timeout_ms: " << client_timeout_ms
           << " end_ms: " << end_time_ms_;
}

void ReadTabletTask::StartRead() {
  if (total_row_num_ == 0) {
    response_->set_status(kTabletNodeOk);
    response_->set_success_num(read_success_num_.Get());
    done_->Run();
    return;
  }

  response_->mutable_detail()->mutable_status()->Reserve(total_row_num_);
  for (int i = 0; i != total_row_num_; ++i) {
    response_->mutable_detail()->mutable_status()->AddAlreadyReserved();
  }

  int64_t max_task_num = FLAGS_tera_tabletnode_parallel_read_task_num;
  int64_t min_rows_per_task = FLAGS_tera_tabletnode_parallel_read_rows_per_task;
  int64_t max_size = max_task_num * min_rows_per_task;
  int64_t rows_per_task;

  if (max_size >= total_row_num_) {
    rows_per_task = min_rows_per_task;
  } else {
    if (max_task_num <= 1) {
      rows_per_task = total_row_num_;
    } else {
      rows_per_task = total_row_num_ / max_task_num + 1;
    }
  }
  int64_t shard_cnt = total_row_num_ / rows_per_task + 1;

  row_results_list_.reserve(shard_cnt);

  int64_t row_to_read = total_row_num_;
  int64_t offset = 0;
  while (row_to_read > 0) {
    row_results_list_.emplace_back();
    auto shard_request = make_shared<ShardRequest>(offset, std::min(rows_per_task, row_to_read),
                                                   &row_results_list_.back());
    row_to_read -= rows_per_task;
    offset += rows_per_task;
    // We split one read request to several shard_request.
    // row_to_read <= 0 means this is the last sharded request(No more rows need
    // to read).
    // So this sharded request is processed in current thread for reducing cost
    // of switching thread.
    // Otherwise, shard_request is added to read_thread_pool.
    if (row_to_read <= 0) {
      DoRead(shard_request);
    } else {
      read_thread_pool_->AddTask(
          std::bind(&ReadTabletTask::DoRead, shared_from_this(), shard_request));
    }
  }
}

void ReadTabletTask::DoRead(std::shared_ptr<ShardRequest> shard_req) {
  bool is_timeout{false};

  auto& row_results = *shard_req->row_results;
  int64_t index = shard_req->offset;
  int64_t end_index = index + shard_req->row_num;

  while (index < end_index) {
    int64_t time_remain_ms = end_time_ms_ - GetTimeStampInMs();
    StatusCode row_status = kTabletNodeOk;

    io::TabletIO* tablet_io = tablet_manager_->GetTablet(
        request_->tablet_name(), request_->row_info_list(index).key(), &row_status);
    if (tablet_io == NULL) {
      response_->mutable_detail()->mutable_status()->Set(index, kKeyNotInRange);
      read_range_error_counter.Inc();
    } else {
      row_results.emplace_back(new RowResult{});
      VLOG(20) << "time_remain_ms: " << time_remain_ms;
      if (tablet_io->ReadCells(request_->row_info_list(index), row_results.back().get(),
                               snapshot_id_, &row_status, time_remain_ms)) {
        read_success_num_.Inc();
      } else {
        if (row_status != kKeyNotExist && row_status != kRPCTimeout) {
          if (row_status == kTabletNodeIsBusy) {
            read_reject_counter.Inc();
          } else {
            read_error_counter.Inc();
          }
        }
        row_results.pop_back();
      }
      tablet_io->DecRef();
      response_->mutable_detail()->mutable_status()->Set(index, row_status);
    }
    if (row_status == kRPCTimeout || has_timeout_.load()) {
      is_timeout = true;
      LOG(WARNING) << "seq_id: " << request_->sequence_id() << " timeout,"
                   << " clinet_timeout_ms: " << request_->client_timeout_ms();
      break;
    }
    ++index;
  }

  if (is_timeout) {
    has_timeout_.store(true);
  }

  FinishShardRequest(shard_req);
}

void ReadTabletTask::FinishShardRequest(const std::shared_ptr<ShardRequest>& shard_req) {
  if (finished_.Add(shard_req->row_num) == total_row_num_) {
    if (has_timeout_.load()) {
      response_->set_status(kRPCTimeout);
      done_->Run();
      return;
    }

    int64_t size = 0;
    for (const auto& row_results : row_results_list_) {
      size += row_results.size();
    }

    response_->mutable_detail()->mutable_row_result()->Reserve(size);
    for (auto& row_results : row_results_list_) {
      for (auto result : row_results) {
        response_->mutable_detail()->add_row_result()->Swap(result.get());
      }
    }
    response_->set_status(kTabletNodeOk);
    response_->set_success_num(read_success_num_.Get());
    done_->Run();
  }
  return;
}

void TabletNodeImpl::ReadTablet(int64_t start_micros, const ReadTabletRequest* request,
                                ReadTabletResponse* response, google::protobuf::Closure* done,
                                ThreadPool* read_thread_pool) {
  auto read_tablet_task = make_shared<ReadTabletTask>(start_micros, tablet_manager_, request,
                                                      response, done, read_thread_pool);

  read_tablet_task->StartRead();
}

void TabletNodeImpl::PersistentCacheGarbageCollect(const std::set<std::string>& inherited_files,
                                                   const std::set<std::string>& active_tablets) {
  std::shared_ptr<leveldb::PersistentCache> p_cache;
  if (!io::GetPersistentCache(&p_cache).ok() || !p_cache) {
    return;
  }
  leveldb::StopWatchMicro timer(leveldb::Env::Default(), true);
  std::vector<std::string> all_keys{p_cache->GetAllKeys()};
  /*
   * all cached tablets/files:
   * ------------------------------------------
   * | active tablets  |   inactive tablets   |
   * |                 |                      |
   * |                 |    all    |    to    |
   * |                 | inherited | *DELETE* |
   * |                 |    files  |          |
   * ------------------------------------------
   * We need to save active tablets' files and inherited files.
   * Try remove files of tablets not on this tabletserver.
   * Here is the gc rule:
   *
   * Key format in persistent cache:  |table_name/tablet_name/lg_num/xxxxxxxx.sst|
   *                                  |          1           |                   |
   * string in active_tablets         |table_name/tablet_name|                   |
   *                                  |                      2                   |
   * string in inherited_files        |table_name/tablet_name/lg_num/xxxxxxxx.sst|
   *
   * If part 1 of persistent cache key doesn't match any string in active tablets,
   * and part 2 of it doesn't match any one in inherited_files, we'll remove it.
   */
  std::unordered_set<std::string> new_delayed_gc_files;
  for (auto& key : all_keys) {
    if (inherited_files.find(key) != inherited_files.end()) {
      // 1. If file name in inherited_files, skip it.
      continue;
    }
    std::vector<std::string> splited_terms;
    SplitString(key, "/", &splited_terms);
    assert(splited_terms.size() > 2);
    // 2. Extract table_name/tablet_name from persistent key.
    std::string tablet_name = splited_terms[0] + "/" + splited_terms[1];
    if (active_tablets.find(tablet_name) != active_tablets.end()) {
      // 3. Skip active tablets' file.
      continue;
    }
    if (delayed_gc_files_.find(key) != delayed_gc_files_.end()) {
      LOG(INFO) << "[Persistent Cache GC] Remove unused file: " << key << ".";
      // 4. If this key has already be delayed for one gc period, remove it.
      p_cache->ForceEvict(key);
    } else {
      LOG(INFO) << "[Persistent Cache GC] Add file: " << key << " to delayed gc files.";
      // 5. Otherwise, it'll be add to delayed_gc_files, waiting for next gc process.
      new_delayed_gc_files.emplace(key);
    }
  }

  std::swap(delayed_gc_files_, new_delayed_gc_files);
  p_cache->GarbageCollect();
  LOG(INFO) << "[Persistent Cache GC] Finished, cost: " << timer.ElapsedMicros() / 1000 << " ms.";
}

void TabletNodeImpl::InitDfsReadThreadLimiter() {
  auto thread_limit =
      static_cast<int64_t>(FLAGS_dfs_read_thread_ratio * FLAGS_tera_tabletnode_read_thread_num);
  thread_limit =
      std::min(static_cast<int64_t>(FLAGS_tera_tabletnode_read_thread_num), thread_limit);
  thread_limit = std::max(static_cast<int64_t>(1), thread_limit);
  leveldb::DfsReadThreadLimiter::Instance().SetLimit(thread_limit);
  LOG(INFO) << "Init dfs read thread limiter with " << thread_limit << ", total thread "
            << FLAGS_tera_tabletnode_read_thread_num;
}

}  // namespace tabletnode
}  // namespace tera
