// Copyright (c) 2015, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TERA_SDK_TABLE_IMPL_H_
#define TERA_SDK_TABLE_IMPL_H_
#include <atomic>
#include <memory>

#include "common/mutex.h"
#include "common/timer.h"
#include "common/thread_pool.h"

#include "leveldb/util/histogram.h"
#include "proto/table_meta.pb.h"
#include "proto/tabletnode_rpc.pb.h"
#include "sdk/client_impl.h"
#include "sdk/sdk_task.h"
#include "sdk/sdk_zk.h"
#include "tera.h"
#include "common/counter.h"

namespace tera {

namespace master {
class MasterClient;
}

namespace tabletnode {
class TabletNodeClient;
}

class RowMutation;
class RowMutationImpl;
class BatchMutation;
class BatchMutationImpl;
class ResultStreamImpl;
class ScanTask;
class ScanDescImpl;
class WriteTabletRequest;
class WriteTabletResponse;
class RowReaderImpl;
class ReadTabletRequest;
class ReadTabletResponse;

class TableImpl : public Table, public std::enable_shared_from_this<TableImpl> {
  friend class MutationCommitBuffer;
  friend class RowMutationImpl;
  friend class RowReaderImpl;
  friend class BatchMutationImpl;

 public:
  TableImpl(const std::string& table_name, ThreadPool* thread_pool,
            std::shared_ptr<ClientImpl> client_impl);

  virtual ~TableImpl();

  virtual RowMutation* NewRowMutation(const std::string& row_key);
  virtual BatchMutation* NewBatchMutation();

  virtual RowReader* NewRowReader(const std::string& row_key);

  virtual void ApplyMutation(RowMutation* row_mu);
  virtual void ApplyMutation(const std::vector<RowMutation*>& row_mutations);
  virtual void ApplyMutation(BatchMutation* batch_mutation);

  virtual void Put(RowMutation* row_mu);
  virtual void Put(const std::vector<RowMutation*>& row_mutations);

  virtual bool Put(const std::string& row_key, const std::string& family,
                   const std::string& qualifier, const std::string& value, ErrorCode* err);
  virtual bool Put(const std::string& row_key, const std::string& family,
                   const std::string& qualifier, const std::string& value, int64_t timestamp,
                   ErrorCode* err);
  virtual bool Put(const std::string& row_key, const std::string& family,
                   const std::string& qualifier, const int64_t value, ErrorCode* err);
  virtual bool Put(const std::string& row_key, const std::string& family,
                   const std::string& qualifier, const std::string& value, int32_t ttl,
                   ErrorCode* err);
  virtual bool Put(const std::string& row_key, const std::string& family,
                   const std::string& qualifier, const std::string& value, int64_t timestamp,
                   int32_t ttl, ErrorCode* err);

  virtual bool Add(const std::string& row_key, const std::string& family,
                   const std::string& qualifier, int64_t delta, ErrorCode* err);
  virtual bool AddInt64(const std::string& row_key, const std::string& family,
                        const std::string& qualifier, int64_t delta, ErrorCode* err);

  virtual bool PutIfAbsent(const std::string& row_key, const std::string& family,
                           const std::string& qualifier, const std::string& value, ErrorCode* err);

  /// 原子操作：追加内容到一个Cell
  virtual bool Append(const std::string& row_key, const std::string& family,
                      const std::string& qualifier, const std::string& value, ErrorCode* err);

  virtual void Get(RowReader* row_reader);
  virtual void Get(const std::vector<RowReader*>& row_readers);
  virtual bool Get(const std::string& row_key, const std::string& family,
                   const std::string& qualifier, std::string* value, ErrorCode* err);
  virtual bool Get(const std::string& row_key, const std::string& family,
                   const std::string& qualifier, int64_t* value, ErrorCode* err);
  virtual bool Get(const std::string& row_key, const std::string& family,
                   const std::string& qualifier, std::string* value, uint64_t snapshot_id,
                   ErrorCode* err);
  virtual bool Get(const std::string& row_key, const std::string& family,
                   const std::string& qualifier, int64_t* value, uint64_t snapshot_id,
                   ErrorCode* err);
  virtual bool Get(const std::string& row_key, const std::string& family,
                   const std::string& qualifier, std::string* value, ErrorCode* err,
                   uint64_t snapshot_id);
  virtual bool Get(const std::string& row_key, const std::string& family,
                   const std::string& qualifier, int64_t* value, ErrorCode* err,
                   uint64_t snapshot_id);

  virtual bool IsPutFinished() { return cur_commit_pending_counter_.Get() == 0; }

  virtual bool IsGetFinished() { return cur_reader_pending_counter_.Get() == 0; }

  virtual ResultStream* Scan(const ScanDescriptor& desc, ErrorCode* err);

  virtual const std::string GetName() { return name_; }

  virtual bool Flush();

  virtual bool CheckAndApply(const std::string& rowkey, const std::string& cf_c,
                             const std::string& value, const RowMutation& row_mu, ErrorCode* err);

  virtual int64_t IncrementColumnValue(const std::string& row, const std::string& family,
                                       const std::string& qualifier, int64_t amount,
                                       ErrorCode* err);

  /// 创建事务
  virtual Transaction* StartRowTransaction(const std::string& row_key);
  /// 提交事务
  virtual void CommitRowTransaction(Transaction* transaction);

  virtual void SetWriteTimeout(int64_t timeout_ms);
  virtual void SetReadTimeout(int64_t timeout_ms);

  virtual bool LockRow(const std::string& rowkey, RowLock* lock, ErrorCode* err);

  virtual bool GetStartEndKeys(std::string* start_key, std::string* end_key, ErrorCode* err);

  virtual bool GetTabletLocation(std::vector<TabletInfo>* tablets, ErrorCode* err);

  virtual bool GetTablet(const std::string& row_key, std::string* tablet);

  virtual bool GetDescriptor(TableDescriptor* desc, ErrorCode* err);

  virtual void SetMaxMutationPendingNum(uint64_t max_pending_num) {
    max_commit_pending_num_ = max_pending_num;
  }
  virtual void SetMaxReaderPendingNum(uint64_t max_pending_num) {
    max_reader_pending_num_ = max_pending_num;
  }

 public:
  bool OpenInternal(std::function<std::string(const std::string&)> hash_method, ErrorCode* err);

  virtual void ScanTabletAsync(ResultStreamImpl* stream);

  void ScanMetaTable(const std::string& key_start, const std::string& key_end);

  bool GetTabletMetaForKey(const std::string& key, TabletMeta* meta);

  uint64_t GetMaxMutationPendingNum() { return max_commit_pending_num_; }
  uint64_t GetMaxReaderPendingNum() { return max_reader_pending_num_; }
  TableSchema GetTableSchema() { return table_schema_; }

  void StatUserPerfCounter(enum SdkTask::TYPE op, ErrorCode::ErrorCodeType code, int64_t cost_time);
  struct PerfCounter {
    int64_t start_time;
    Counter rpc_r;      // 读取的耗时
    Counter rpc_r_cnt;  // 读取的次数

    Counter rpc_w;      // 写入的耗时
    Counter rpc_w_cnt;  // 写入的次数

    Counter rpc_s;      // scan的耗时
    Counter rpc_s_cnt;  // scan的次数

    Counter user_callback;      // 运行用户callback的耗时
    Counter user_callback_cnt;  // 运行用户callback的次数

    Counter get_meta;      // 更新meta的耗时
    Counter get_meta_cnt;  // 更新meta的次数

    Counter mutate_cnt;                // 分发mutation的次数
    Counter mutate_ok_cnt;             // mutation回调成功的次数
    Counter mutate_fail_cnt;           // mutation回调失败的次数
    Counter mutate_range_cnt;          // mutation回调失败-原因为not in range
    Counter mutate_timeout_cnt;        // mutation在sdk队列中超时
    Counter mutate_queue_timeout_cnt;  // mutation在sdk队列中超时，且之前从未被重试过

    Counter reader_cnt;                // 分发reader的次数
    Counter reader_ok_cnt;             // reader回调成功的次数
    Counter reader_fail_cnt;           // reader回调失败的次数
    Counter reader_range_cnt;          // reader回调失败-原因为not in range
    Counter reader_timeout_cnt;        // reader在sdk队列中超时
    Counter reader_queue_timeout_cnt;  // raader在sdk队列中超时，且之前从未被重试过

    Counter user_mu_cnt;
    Counter user_mu_suc;
    Counter user_mu_fail;
    ::leveldb::Histogram hist_mu_cost;

    Counter user_read_cnt;
    Counter user_read_suc;
    Counter user_read_notfound;
    Counter user_read_fail;
    ::leveldb::Histogram hist_read_cost;

    ::leveldb::Histogram hist_async_cost;
    Counter meta_sched_cnt;
    Counter meta_update_cnt;
    Counter total_task_cnt;
    Counter total_commit_cnt;

    void DoDumpPerfCounterLog(const std::string& log_prefix);

    Counter GetTimeoutCnt(SdkTask* task) {
      switch (task->Type()) {
        case SdkTask::READ:
          return reader_timeout_cnt;
        case SdkTask::MUTATION:
        case SdkTask::BATCH_MUTATION:
          return mutate_timeout_cnt;
        default:
          abort();
      }
    }

    Counter GetRangeCnt(SdkTask* task) {
      switch (task->Type()) {
        case SdkTask::READ:
          return reader_range_cnt;
        case SdkTask::MUTATION:
        case SdkTask::BATCH_MUTATION:
          return mutate_range_cnt;
        default:
          abort();
      }
    }

    Counter GetTaskFailCnt(SdkTask::TYPE type) {
      switch (type) {
        case SdkTask::READ:
          return reader_range_cnt;
        case SdkTask::MUTATION:
        case SdkTask::BATCH_MUTATION:
          return mutate_range_cnt;
        default:
          abort();
      }
    }

    Counter GetQueueTimeoutCnt(SdkTask* task) {
      switch (task->Type()) {
        case SdkTask::READ:
          return reader_queue_timeout_cnt;
        case SdkTask::MUTATION:
        case SdkTask::BATCH_MUTATION:
          return mutate_queue_timeout_cnt;
        default:
          abort();
      }
    }

    Counter GetTaskCnt(SdkTask* task) {
      switch (task->Type()) {
        case SdkTask::READ:
          return reader_cnt;
        case SdkTask::MUTATION:
        case SdkTask::BATCH_MUTATION:
          return mutate_cnt;
        default:
          abort();
      }
    }

    PerfCounter() { start_time = get_micros(); }
  };

  bool IsHashTable() override { return is_hash_table_.load(); }
  std::function<std::string(const std::string&)> GetHashMethod() override { return hash_method_; }

 private:
  bool ScanTabletNode(const TabletMeta& tablet_meta, const std::string& key_start,
                      const std::string& key_end, std::vector<KeyValuePair>* kv_list,
                      ErrorCode* err);

  void DistributeTasks(const std::vector<SdkTask*>& task_list, bool called_by_user,
                       SdkTask::TYPE task_type);

  // 通过异步RPC将mutation提交至TS
  void CommitMutations(const std::string& server_addr, std::vector<RowMutationImpl*>& mu_list);

  void CommitBatchMutations(const std::string& server_addr,
                            std::vector<BatchMutationImpl*>& mu_list);

  // mutate RPC回调
  static void MutateCallBackWrapper(std::weak_ptr<TableImpl> weak_ptr_table,
                                    std::vector<int64_t>* mu_id_list, WriteTabletRequest* request,
                                    WriteTabletResponse* response, bool failed, int error_code);
  void MutateCallBack(std::vector<int64_t>* mu_id_list, WriteTabletRequest* request,
                      WriteTabletResponse* response, bool failed, int error_code);

  static void BatchMutateCallBackWrapper(std::weak_ptr<TableImpl> weak_ptr_table,
                                         std::vector<int64_t>* mu_id_list,
                                         WriteTabletRequest* request, WriteTabletResponse* response,
                                         bool failed, int error_code);

  void BatchMutateCallBack(std::vector<int64_t>* mu_id_list, WriteTabletRequest* request,
                           WriteTabletResponse* response, bool failed, int error_code);

  void TaskTimeout(SdkTask* sdk_task);

  // 将一批reader根据rowkey分配给各个TS
  void DistributeReaders(const std::vector<RowReaderImpl*>& row_reader_list, bool called_by_user);

  // 通过异步RPC将reader提交至TS
  void CommitReaders(const std::string& server_addr, std::vector<RowReaderImpl*>& reader_list);

  void DistributeTasksById(std::vector<int64_t>* task_id_list, SdkTask::TYPE task_type);

  void DistributeDelayTasks(const std::map<uint32_t, std::vector<int64_t>*>& retry_times_list,
                            SdkTask::TYPE task_type);

  void CollectFailedTasks(int64_t task_id, SdkTask::TYPE type, StatusCode err,
                          std::vector<SdkTask*>* not_in_range_list,
                          std::map<uint32_t, std::vector<int64_t>*>* retry_times_list);

  // reader RPC回调
  static void ReaderCallBackWrapper(std::weak_ptr<TableImpl> weak_ptr_table,
                                    std::vector<int64_t>* reader_id_list,
                                    ReadTabletRequest* request, ReadTabletResponse* response,
                                    bool failed, int error_code);

  void ReaderCallBack(std::vector<int64_t>* reader_id_list, ReadTabletRequest* request,
                      ReadTabletResponse* response, bool failed, int error_code);

  void PackSdkTasks(const std::string& server_addr, std::vector<SdkTask*>& task_list,
                    SdkTask::TYPE task_type);
  void TaskBatchTimeout(SdkTask* task);
  void CommitTasksById(const std::string& server_addr, std::vector<int64_t>& task_id_list,
                       SdkTask::TYPE task_type);

  void ScanTabletAsync(ScanTask* scan_task, bool called_by_user);

  void CommitScan(ScanTask* scan_task, const std::string& server_addr);

  static void ScanCallBackWrapper(std::weak_ptr<TableImpl> weak_ptr_table, ScanTask* scan_task,
                                  ScanTabletRequest* request, ScanTabletResponse* response,
                                  bool failed, int error_code);
  void ScanCallBack(ScanTask* scan_task, ScanTabletRequest* request, ScanTabletResponse* response,
                    bool failed, int error_code);

  void BreakRequest(int64_t task_id);
  void BreakScan(ScanTask* scan_task);

  enum TabletMetaStatus { NORMAL, DELAY_UPDATE, WAIT_UPDATE, UPDATING };
  struct TabletMetaNode {
    TabletMeta meta;
    int64_t update_time;
    TabletMetaStatus status;

    TabletMetaNode() : update_time(0), status(NORMAL) {}
  };

  bool GetTabletAddrOrScheduleUpdateMeta(const std::string& row, SdkTask* request,
                                         std::string* server_addr);

  TabletMetaNode* GetTabletMetaNodeForKey(const std::string& key);

  void DelayUpdateMeta(const std::string& start_key, const std::string& end_key);

  void UpdateMetaAsync();

  void ScanMetaTableAsync(const std::string& key_start, const std::string& key_end,
                          const std::string& expand_key_end, bool zk_access);

  void ScanMetaTableAsyncInLock(const std::string& key_start, const std::string& key_end,
                                const std::string& expand_key_end, bool zk_access);
  static void ScanMetaTableCallBackWrapper(std::weak_ptr<TableImpl> weak_ptr_table,
                                           std::string key_start, std::string key_end,
                                           std::string expand_key_end, int64_t start_time,
                                           ScanTabletRequest* request, ScanTabletResponse* response,
                                           bool failed, int error_code);
  void ScanMetaTableCallBack(std::string key_start, std::string key_end, std::string expand_key_end,
                             int64_t start_time, ScanTabletRequest* request,
                             ScanTabletResponse* response, bool failed, int error_code);

  void UpdateTabletMetaList(const TabletMeta& meta);

  void GiveupUpdateTabletMeta(const std::string& key_start, const std::string& key_end);

  void WakeUpPendingRequest(const TabletMetaNode& node);

  void ScheduleUpdateMeta(const std::string& row, int64_t meta_timestamp);

  bool UpdateTableMeta(ErrorCode* err);
  void ReadTableMetaAsync(ErrorCode* ret_err, int32_t retry_times, bool zk_access);

  static void ReadTableMetaCallBackWrapper(std::weak_ptr<TableImpl> weak_ptr_table,
                                           ErrorCode* ret_err, int32_t retry_times,
                                           ReadTabletRequest* request, ReadTabletResponse* response,
                                           bool failed, int error_code);
  void ReadTableMetaCallBack(ErrorCode* ret_err, int32_t retry_times, ReadTabletRequest* request,
                             ReadTabletResponse* response, bool failed, int error_code);
  bool RestoreCookie();
  void EnableCookieUpdateTimer();
  void DumpCookie();
  void DoDumpCookie();
  std::string GetCookieFileName(const std::string& tablename, const std::string& cluster_id,
                                int64_t create_time);
  std::string GetCookieFilePathName();
  std::string GetCookieLockFilePathName();
  void DeleteLegacyCookieLockFile(const std::string& lock_file, int timeout_seconds);
  void CloseAndRemoveCookieLockFile(int lock_fd, const std::string& cookie_lock_file);

  void DumpPerfCounterLogDelay();
  void DoDumpPerfCounterLog();

  void DelayTaskWrapper(ThreadPool::Task task, int64_t task_id);
  int64_t AddDelayTask(int64_t delay_time, ThreadPool::Task task);
  void ClearDelayTask();

 private:
  TableImpl(const TableImpl&);
  void operator=(const TableImpl&);

  struct TaskBatch : public SdkTask {
    uint64_t byte_size = 0;
    std::string server_addr;
    SdkTask::TYPE type;
    Mutex* mutex = nullptr;
    std::map<std::string, TaskBatch*>* task_batch_map = nullptr;
    std::vector<int64_t>* row_id_list = nullptr;

    TaskBatch() : SdkTask(SdkTask::TASKBATCH) {}
    virtual bool IsAsync() { return false; }
    virtual uint32_t Size() { return 0; }
    virtual void SetTimeOut(int64_t timeout) {}
    virtual int64_t TimeOut() { return 0; }
    virtual void Wait() {}
    virtual void SetError(ErrorCode::ErrorCodeType err, const std::string& reason) {}
    virtual std::string InternalRowKey() { return server_addr; }
    // task batch not implement this interface
    virtual int64_t GetCommitTimes() { return 0; }
    // task batch not implement this interface
    virtual void RunCallback() { abort(); }
  };

  std::string name_;
  int64_t create_time_;
  uint64_t last_sequence_id_;
  uint32_t write_timeout_;
  uint32_t read_timeout_;

  std::shared_ptr<ClientImpl> client_impl_;
  std::shared_ptr<auth::AccessBuilder> access_builder_;

  mutable Mutex mutation_batch_mutex_;
  mutable Mutex reader_batch_mutex_;
  uint32_t commit_size_;
  uint64_t write_commit_timeout_;
  uint64_t read_commit_timeout_;
  std::map<std::string, TaskBatch*> mutation_batch_map_;
  std::map<std::string, TaskBatch*> reader_batch_map_;
  Counter cur_commit_pending_counter_;
  Counter cur_reader_pending_counter_;
  int64_t max_commit_pending_num_;
  int64_t max_reader_pending_num_;

  // meta management
  mutable Mutex meta_mutex_;
  common::CondVar meta_cond_;
  std::map<std::string, std::list<int64_t>> pending_task_id_list_;
  uint32_t meta_updating_count_;
  std::map<std::string, TabletMetaNode> tablet_meta_list_;
  // end of meta management

  // table meta managerment
  mutable Mutex table_meta_mutex_;
  common::CondVar table_meta_cond_;
  bool table_meta_updating_;
  TableSchema table_schema_;
  // end of table meta managerment

  SdkTimeoutManager task_pool_;
  Counter next_task_id_;

  master::MasterClient* master_client_;
  tabletnode::TabletNodeClient* tabletnode_client_;

  ThreadPool* thread_pool_;
  mutable Mutex delay_task_id_mutex_;
  std::set<int64_t> delay_task_ids_;
  /// cluster_ could cache the master_addr & root_table_addr.
  /// if there is no cluster_,
  ///    we have to access zookeeper whenever we need master_addr or
  ///    root_table_addr.
  /// if there is cluster_,
  ///    we save master_addr & root_table_addr in cluster_, access zookeeper
  ///    only once.
  sdk::ClusterFinder* cluster_;
  bool cluster_private_;

  PerfCounter perf_counter_;  // calc time consumption, for performance analysis

  std::atomic<bool> is_hash_table_{false};
  std::function<std::string(const std::string&)> hash_method_;

  // server_addr, rpc_timeout_duration
  // Records the last time(ms) of the server response with non-rpctimeout.
  std::unordered_map<std::string, int64_t> rpc_timeout_duration_;
  mutable Mutex rpc_timeout_duration_mutex_;
};

class TableWrapper : public Table {
 public:
  explicit TableWrapper(const std::shared_ptr<TableImpl>& impl) : impl_(impl) {}
  virtual ~TableWrapper() {}
  virtual RowMutation* NewRowMutation(const std::string& row_key) {
    return impl_->NewRowMutation(row_key);
  }
  virtual BatchMutation* NewBatchMutation() { return impl_->NewBatchMutation(); }
  virtual RowReader* NewRowReader(const std::string& row_key) {
    return impl_->NewRowReader(row_key);
  }
  virtual void Put(RowMutation* row_mu) { impl_->Put(row_mu); }
  virtual void Put(const std::vector<RowMutation*>& row_mu_list) { impl_->Put(row_mu_list); }
  virtual void ApplyMutation(RowMutation* row_mu) { impl_->ApplyMutation(row_mu); }
  virtual void ApplyMutation(const std::vector<RowMutation*>& row_mu_list) {
    impl_->ApplyMutation(row_mu_list);
  }
  virtual void ApplyMutation(BatchMutation* batch_mutation) {
    impl_->ApplyMutation(batch_mutation);
  }
  virtual bool Put(const std::string& row_key, const std::string& family,
                   const std::string& qualifier, const std::string& value, ErrorCode* err) {
    return impl_->Put(row_key, family, qualifier, value, err);
  }
  virtual bool Put(const std::string& row_key, const std::string& family,
                   const std::string& qualifier, const int64_t value, ErrorCode* err) {
    return impl_->Put(row_key, family, qualifier, value, err);
  }
  virtual bool Put(const std::string& row_key, const std::string& family,
                   const std::string& qualifier, const std::string& value, int32_t ttl,
                   ErrorCode* err) {
    return impl_->Put(row_key, family, qualifier, value, ttl, err);
  }
  virtual bool Put(const std::string& row_key, const std::string& family,
                   const std::string& qualifier, const std::string& value, int64_t timestamp,
                   int32_t ttl, ErrorCode* err) {
    return impl_->Put(row_key, family, qualifier, value, timestamp, ttl, err);
  }
  virtual bool Add(const std::string& row_key, const std::string& family,
                   const std::string& qualifier, int64_t delta, ErrorCode* err) {
    return impl_->Add(row_key, family, qualifier, delta, err);
  }
  virtual bool AddInt64(const std::string& row_key, const std::string& family,
                        const std::string& qualifier, int64_t delta, ErrorCode* err) {
    return impl_->AddInt64(row_key, family, qualifier, delta, err);
  }

  virtual bool PutIfAbsent(const std::string& row_key, const std::string& family,
                           const std::string& qualifier, const std::string& value, ErrorCode* err) {
    return impl_->PutIfAbsent(row_key, family, qualifier, value, err);
  }

  virtual bool Append(const std::string& row_key, const std::string& family,
                      const std::string& qualifier, const std::string& value, ErrorCode* err) {
    return impl_->Append(row_key, family, qualifier, value, err);
  }
  virtual void Get(RowReader* row_reader) { impl_->Get(row_reader); }
  virtual void Get(const std::vector<RowReader*>& row_readers) { impl_->Get(row_readers); }
  virtual bool Get(const std::string& row_key, const std::string& family,
                   const std::string& qualifier, std::string* value, ErrorCode* err) {
    return impl_->Get(row_key, family, qualifier, value, err);
  }
  virtual bool Get(const std::string& row_key, const std::string& family,
                   const std::string& qualifier, int64_t* value, ErrorCode* err) {
    return impl_->Get(row_key, family, qualifier, value, err);
  }
  virtual bool Get(const std::string& row_key, const std::string& family,
                   const std::string& qualifier, std::string* value, ErrorCode* err,
                   uint64_t snapshot_id) {
    return impl_->Get(row_key, family, qualifier, value, snapshot_id, err);
  }
  virtual bool Get(const std::string& row_key, const std::string& family,
                   const std::string& qualifier, std::string* value, uint64_t snapshot_id,
                   ErrorCode* err) {
    return impl_->Get(row_key, family, qualifier, value, snapshot_id, err);
  }
  virtual bool Get(const std::string& row_key, const std::string& family,
                   const std::string& qualifier, int64_t* value, ErrorCode* err,
                   uint64_t snapshot_id) {
    return impl_->Get(row_key, family, qualifier, value, snapshot_id, err);
  }
  virtual bool Get(const std::string& row_key, const std::string& family,
                   const std::string& qualifier, int64_t* value, uint64_t snapshot_id,
                   ErrorCode* err) {
    return impl_->Get(row_key, family, qualifier, value, snapshot_id, err);
  }

  virtual bool IsPutFinished() { return impl_->IsPutFinished(); }
  virtual bool IsGetFinished() { return impl_->IsGetFinished(); }

  virtual ResultStream* Scan(const ScanDescriptor& desc, ErrorCode* err) {
    return impl_->Scan(desc, err);
  }

  virtual const std::string GetName() { return impl_->GetName(); }

  virtual bool Flush() { return impl_->Flush(); }
  virtual bool CheckAndApply(const std::string& rowkey, const std::string& cf_c,
                             const std::string& value, const RowMutation& row_mu, ErrorCode* err) {
    return impl_->CheckAndApply(rowkey, cf_c, value, row_mu, err);
  }
  virtual int64_t IncrementColumnValue(const std::string& row, const std::string& family,
                                       const std::string& qualifier, int64_t amount,
                                       ErrorCode* err) {
    return impl_->IncrementColumnValue(row, family, qualifier, amount, err);
  }
  virtual Transaction* StartRowTransaction(const std::string& row_key) {
    return impl_->StartRowTransaction(row_key);
  }
  virtual void CommitRowTransaction(Transaction* transaction) {
    impl_->CommitRowTransaction(transaction);
  }
  virtual void SetWriteTimeout(int64_t timeout_ms) { impl_->SetWriteTimeout(timeout_ms); }
  virtual void SetReadTimeout(int64_t timeout_ms) { impl_->SetReadTimeout(timeout_ms); }

  virtual bool LockRow(const std::string& rowkey, RowLock* lock, ErrorCode* err) {
    return impl_->LockRow(rowkey, lock, err);
  }

  virtual bool GetStartEndKeys(std::string* start_key, std::string* end_key, ErrorCode* err) {
    return impl_->GetStartEndKeys(start_key, end_key, err);
  }

  virtual bool GetTabletLocation(std::vector<TabletInfo>* tablets, ErrorCode* err) {
    return impl_->GetTabletLocation(tablets, err);
  }
  virtual bool GetDescriptor(TableDescriptor* desc, ErrorCode* err) {
    return impl_->GetDescriptor(desc, err);
  }

  virtual void SetMaxMutationPendingNum(uint64_t max_pending_num) {
    impl_->SetMaxMutationPendingNum(max_pending_num);
  }
  virtual void SetMaxReaderPendingNum(uint64_t max_pending_num) {
    impl_->SetMaxReaderPendingNum(max_pending_num);
  }

  virtual bool IsHashTable() { return impl_->IsHashTable(); }

  virtual std::function<std::string(const std::string&)> GetHashMethod() {
    return impl_->GetHashMethod();
  }

  virtual bool GetTablet(const std::string& row_key, std::string* tablet) override {
    return impl_->GetTablet(row_key, tablet);
  }

  std::shared_ptr<TableImpl> GetTableImpl() { return impl_; }

 private:
  std::shared_ptr<TableImpl> impl_;
};

}  // namespace tera

#endif  // TERA_SDK_TABLE_IMPL_H_
