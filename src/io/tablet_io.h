// Copyright (c) 2015, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TERA_IO_TABLET_IO_H_
#define TERA_IO_TABLET_IO_H_

#include <functional>
#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "common/base/scoped_ptr.h"
#include "common/metric/metric_counter.h"
#include "common/mutex.h"
#include "io/tablet_scanner.h"
#include "leveldb/db.h"
#include "leveldb/options.h"
#include "leveldb/raw_key_operator.h"
#include "leveldb/slice.h"
#include "leveldb/write_batch.h"
#include "proto/proto_helper.h"
#include "proto/status_code.pb.h"
#include "proto/table_meta.pb.h"
#include "proto/table_schema.pb.h"
#include "proto/tabletnode_rpc.pb.h"
#include "types.h"
#include "common/counter.h"

namespace tera {

// metric name constants
const char* const kLowReadCellMetricName = "tera_ts_tablet_low_read_cell_count";
const char* const kScanRowsMetricName = "tera_ts_tablet_scan_row_count";
const char* const kScanKvsMetricName = "tera_ts_tablet_scan_kv_count";
const char* const kScanThroughPutMetricName = "tera_ts_tablet_scan_through_put";
const char* const kReadRowsMetricName = "tera_ts_tablet_read_row_count";
const char* const kReadKvsMetricName = "tera_ts_tablet_read_kv_count";
const char* const kReadThroughPutMetricName = "tera_ts_tablet_read_through_put";
const char* const kWriteRowsMetricName = "tera_ts_tablet_write_row_count";
const char* const kWriteKvsMetricName = "tera_ts_tablet_write_kv_count";
const char* const kWriteThroughPutMetricName = "tera_ts_tablet_write_through_put";
const char* const kWriteRejectRowsMetricName = "tera_ts_tablet_write_reject_row_count";

namespace io {

class TabletWriter;
struct ScanOptions;
struct ScanContext;
class ScanContextManager;

std::string MetricLabelToString(const std::string& tablet_path);

class TabletIO {
public:
    enum CompactionType {
        kManualCompaction = 1,
        kMinorCompaction = 2,
    };

    enum TabletStatus {
        kNotInit = kTabletNotInit,
        kReady = kTabletReady,
        kOnLoad = kTabletOnLoad,
        kOnSplit = kTabletOnSplit,
        kSplited = kTabletSplited,
        kUnLoading = kTabletUnLoading,
        kUnLoading2 = kTabletUnLoading2
    };

    struct StatCounter {
        const std::string label;
        tera::MetricCounter low_read_cell;
        tera::MetricCounter scan_rows;
        tera::MetricCounter scan_kvs;
        tera::MetricCounter scan_size;
        tera::MetricCounter read_rows;
        tera::MetricCounter read_kvs;
        tera::MetricCounter read_size;
        tera::MetricCounter write_rows;
        tera::MetricCounter write_kvs;
        tera::MetricCounter write_size;
        tera::MetricCounter write_reject_rows;

        StatCounter(const std::string& tablet_path)
            : label(MetricLabelToString(tablet_path)),
              low_read_cell(tera::kLowReadCellMetricName, label, {SubscriberType::QPS}),
              scan_rows(tera::kScanRowsMetricName, label, {SubscriberType::QPS}),
              scan_kvs(tera::kScanKvsMetricName, label, {SubscriberType::QPS}),
              scan_size(tera::kScanThroughPutMetricName, label, {SubscriberType::THROUGHPUT}),
              read_rows(tera::kReadRowsMetricName, label, {SubscriberType::QPS}),
              read_kvs(tera::kReadKvsMetricName, label, {SubscriberType::QPS}),
              read_size(tera::kReadThroughPutMetricName, label, {SubscriberType::THROUGHPUT}),
              write_rows(tera::kWriteRowsMetricName, label, {SubscriberType::QPS}),
              write_kvs(tera::kWriteKvsMetricName, label, {SubscriberType::QPS}),
              write_size(tera::kWriteThroughPutMetricName, label, {SubscriberType::THROUGHPUT}),
              write_reject_rows(tera::kWriteRejectRowsMetricName, label, {SubscriberType::QPS}) {}
    };

    typedef std::function<void (std::vector<const RowMutationSequence*>*,
                                std::vector<StatusCode>*)> WriteCallback;

    friend std::ostream& operator << (std::ostream& o, const TabletIO& tablet_io);

public:
    TabletIO(const std::string& key_start, const std::string& key_end,
             const std::string& path);
    virtual ~TabletIO();

    // for testing
    void SetMockEnv(leveldb::Env* e);

    std::string GetTableName() const;
    std::string GetTablePath() const;
    std::string GetStartKey() const;
    std::string GetEndKey() const;
    const std::string& GetMetricLabel() const;
    virtual CompactStatus GetCompactStatus() const;
    virtual TableSchema GetSchema() const;
    RawKey RawKeyType() const;
    bool KvOnly() const { return kv_only_; }
    StatCounter& GetCounter();
    // Set independent cache for memory table.
    void SetMemoryCache(leveldb::Cache* cache);
    // tablet
    virtual bool Load(const TableSchema& schema,
                      const std::string& path,
                      const std::vector<uint64_t>& parent_tablets,
                      const std::set<std::string>& ignore_err_lgs,
                      std::map<uint64_t, uint64_t> snapshots,
                      std::map<uint64_t, uint64_t> rollbacks,
                      leveldb::Logger* logger = NULL,
                      leveldb::Cache* block_cache = NULL,
                      leveldb::TableCache* table_cache = NULL,
                      StatusCode* status = NULL);
    virtual bool Unload(StatusCode* status = NULL);
    virtual bool Split(std::string* split_key, StatusCode* status = NULL);
    virtual bool Compact(int lg_no = -1, StatusCode* status = NULL, CompactionType type = kManualCompaction);
    bool Destroy(StatusCode* status = NULL);
    virtual bool GetDataSize(uint64_t* size, std::vector<uint64_t>* lgsize = NULL,
                             StatusCode* status = NULL);
    virtual bool AddInheritedLiveFiles(std::vector<std::set<uint64_t> >* live);

    bool IsBusy();
    bool Workload(double* write_workload);

    bool SnapshotIDToSeq(uint64_t snapshot_id, uint64_t* snapshot_sequence);

    virtual bool Read(const leveldb::Slice& key, std::string* value,
                      uint64_t snapshot_id = 0, StatusCode* status = NULL);

    // read a row
    virtual bool ReadCells(const RowReaderInfo& row_reader, RowResult* value_list,
                           uint64_t snapshot_id = 0, StatusCode* status = NULL,
                           int64_t timeout_ms = std::numeric_limits<int64_t>::max());
    /// scan from leveldb return ture means complete flase means not complete
    bool LowLevelScan(const std::string& start_tera_key,
                      const std::string& end_row_key,
                      const ScanOptions& scan_options,
                      RowResult* value_list,
                      KeyValuePair* next_start_point,
                      uint32_t* read_row_count,
                      uint32_t* read_bytes,
                      bool* is_complete,
                      StatusCode* status = NULL);

    bool LowLevelSeek(const std::string& row_key, const ScanOptions& scan_options,
                      RowResult* value_list, StatusCode* status = NULL);

    bool WriteOne(const std::string& key, const std::string& value,
                  bool sync = true, StatusCode* status = NULL);
    bool WriteBatch(leveldb::WriteBatch* batch, bool disable_wal = false, bool sync = true,
                    StatusCode* status = NULL);
    bool Write(std::vector<const RowMutationSequence*>* row_mutation_vec,
               std::vector<StatusCode>* status_vec, bool is_instant,
               WriteCallback callback, StatusCode* status = NULL);

    virtual bool Scan(const ScanOption& option, KeyValueList* kv_list,
                      bool* complete, StatusCode* status = NULL);

    virtual bool ScanRows(const ScanTabletRequest* request,
                          ScanTabletResponse* response,
                          google::protobuf::Closure* done);

    uint64_t GetSnapshot(uint64_t id, uint64_t snapshot_sequence,
                         StatusCode* status = NULL);
    bool ReleaseSnapshot(uint64_t snapshot_id,  StatusCode* status = NULL);
    void ListSnapshot(std::vector<uint64_t>* snapshot_id);

    uint64_t Rollback(uint64_t snapshot_id, StatusCode* status);

    uint32_t GetLGidByCFName(const std::string& cfname);

    const leveldb::RawKeyOperator* GetRawKeyOperator();

    void SetStatus(TabletStatus status);
    TabletStatus GetStatus();

    int32_t AddRef();
    int32_t DecRef();
    int32_t GetRef() const;

    static bool FindAverageKey(const std::string& start, const std::string& end,
                               std::string* res);
    void ProcessScan(ScanContext* context);
    void ApplySchema(const TableSchema& schema);

    bool ShouldForceUnloadOnError();

    bool GetDBStatus(tera::TabletStatus* tablet_status, bool slow_check);

private:
    friend class TabletWriter;
    friend class ScanConextManager;
    bool WriteWithoutLock(const std::string& key, const std::string& value,
                          bool sync = false, StatusCode* status = NULL);
//     int64_t GetDataSizeWithoutLock(StatusCode* status = NULL);

    void SetupOptionsForLG(const std::set<std::string>& ignore_err_lgs);
    void TearDownOptionsForLG();
    void IndexingCfToLG();

    void SetupIteratorOptions(const ScanOptions& scan_options,
                              leveldb::ReadOptions* leveldb_opts);
    void SetupSingleRowIteratorOptions(const std::string& row_key,
                                       leveldb::ReadOptions* opts);
    void TearDownIteratorOptions(leveldb::ReadOptions* opts);

    void ProcessRowBuffer(std::list<KeyValuePair>& row_buf,
                          const ScanOptions& scan_options,
                          RowResult* value_list,
                          uint32_t* buffer_size,
                          int64_t* number_limit);

    StatusCode InitedScanIterator(const std::string& start_tera_key,
                                  const std::string& end_row_key,
                                  const ScanOptions& scan_options,
                                  leveldb::Iterator** scan_it);

    bool ScanRowsRestricted(const ScanTabletRequest* request,
                            ScanTabletResponse* response,
                            google::protobuf::Closure* done);
    // tablet scanner
    bool HandleScan(const ScanTabletRequest* request,
                    ScanTabletResponse* response,
                    google::protobuf::Closure* done);

    void SetupScanInternalTeraKey(const ScanTabletRequest* request,
                                  std::string* start_tera_key,
                                  std::string* end_row_key);
    void SetupScanRowOptions(const ScanTabletRequest* request,
                             ScanOptions* scan_options);

    bool LowLevelScan(const std::string& start_tera_key,
                      const std::string& end_row_key,
                      const ScanOptions& scan_options,
                      leveldb::Iterator* it,
                      ScanContext* scan_context,
                      RowResult* value_list,
                      KeyValuePair* next_start_point,
                      uint32_t* read_row_count,
                      uint32_t* read_bytes,
                      bool* is_complete,
                      StatusCode* status);

    void MakeKvPair(leveldb::Slice key, leveldb::Slice col, leveldb::Slice qual,
                    int64_t ts, leveldb::Slice value, KeyValuePair* kv);

    bool ParseRowKey(const std::string& tera_key, std::string* row_key);
    bool ShouldFilterRowBuffer(std::list<KeyValuePair>& row_buf,
                               const ScanOptions& scan_options);

    bool ScanWithFilter(const ScanOptions& scan_options);
    bool IsCompleteRow(const std::list<KeyValuePair>& row_buf,
                       leveldb::Iterator* it);
    bool ShouldFilterRow(const ScanOptions& scan_options,
                           const std::list<KeyValuePair>& row_buf,
                           leveldb::Iterator* it);
    void GotoNextRow(const std::list<KeyValuePair>& row_buf,
                     leveldb::Iterator* it,
                     KeyValuePair* next);
    void SetSchema(const TableSchema& schema);

    bool PutIfAbsentCheck(const std::string& row_key, const Mutation& mutation);

    bool SingleRowTxnCheck(const std::string& row_key,
                           const SingleRowTxnReadInfo& txn_read_info,
                           StatusCode* status);

private:
    mutable Mutex mutex_;
    TabletWriter* async_writer_;
    ScanContextManager* scan_context_manager_;

    std::string tablet_path_;
    const std::string start_key_;
    const std::string end_key_;
    const std::string short_path_;
    std::string raw_start_key_;
    std::string raw_end_key_;
    CompactStatus compact_status_;

    TabletStatus status_;
    tera::TabletStatus tablet_status_; // check wether db corruption
    volatile int32_t ref_count_;
    volatile int32_t db_ref_count_;
    leveldb::Options ldb_options_;
    leveldb::DB* db_;
    leveldb::Cache* m_memory_cache;
    TableSchema table_schema_;
    bool kv_only_;
    std::map<uint64_t, uint64_t> id_to_snapshot_num_;
    std::map<uint64_t, uint64_t> rollbacks_;

    const leveldb::RawKeyOperator* key_operator_;

    std::map<std::string, uint32_t> cf_lg_map_;
    std::map<std::string, uint32_t> lg_id_map_;
    StatCounter counter_;
    mutable Mutex schema_mutex_;

    leveldb::Env* mock_env_; // mock env for testing
};

} // namespace io
} // namespace tera

#endif // TERA_IO_TABLET_IO_H_
