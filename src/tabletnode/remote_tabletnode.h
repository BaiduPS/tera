// Copyright (c) 2015, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TERA_TABLETNODE_REMOTE_TABLETNODE_H_
#define TERA_TABLETNODE_REMOTE_TABLETNODE_H_

#include <mutex>
#include "common/base/scoped_ptr.h"
#include "common/thread_pool.h"
#include "common/request_done_wrapper.h"

#include "proto/tabletnode_rpc.pb.h"
#include "tabletnode/rpc_schedule.h"
#include "utils/rpc_timer_list.h"
#include "access/access_entry.h"
#include "quota/quota_entry.h"

namespace tera {
namespace tabletnode {

class TabletNodeImpl;

class ReadDoneWrapper final : public RequestDoneWrapper {
 public:
  static google::protobuf::Closure* NewInstance(int64_t start_micros,
                                                const ReadTabletRequest* request,
                                                ReadTabletResponse* response,
                                                google::protobuf::Closure* done,
                                                std::shared_ptr<quota::QuotaEntry> quota_entry) {
    return new ReadDoneWrapper(start_micros, request, response, done, quota_entry);
  }

  virtual void Run() override;

  virtual ~ReadDoneWrapper() {}

 protected:
  // Just Can Create on Heap;
  ReadDoneWrapper(int64_t start_micros, const ReadTabletRequest* request,
                  ReadTabletResponse* response, google::protobuf::Closure* done,
                  std::shared_ptr<quota::QuotaEntry> quota_entry)
      : RequestDoneWrapper(done),
        start_micros_(start_micros),
        request_(request),
        response_(response),
        quota_entry_(quota_entry) {}

  int64_t start_micros_;
  const ReadTabletRequest* request_;
  ReadTabletResponse* response_;
  std::shared_ptr<quota::QuotaEntry> quota_entry_;
};

class WriteDoneWrapper final : public RequestDoneWrapper {
 public:
  static google::protobuf::Closure* NewInstance(int64_t start_micros,
                                                const WriteTabletRequest* request,
                                                WriteTabletResponse* response,
                                                google::protobuf::Closure* done) {
    return new WriteDoneWrapper(start_micros, request, response, done);
  }

  virtual void Run() override;

  virtual ~WriteDoneWrapper() {}

 protected:
  // Just Can Create on Heap;
  WriteDoneWrapper(int64_t start_micros, const WriteTabletRequest* request,
                   WriteTabletResponse* response, google::protobuf::Closure* done)
      : RequestDoneWrapper(done),
        start_micros_(start_micros),
        request_(request),
        response_(response) {}

  int64_t start_micros_;
  const WriteTabletRequest* request_;
  WriteTabletResponse* response_;
};

class ScanDoneWrapper final : public RequestDoneWrapper {
 public:
  static google::protobuf::Closure* NewInstance(int64_t start_micros,
                                                const ScanTabletRequest* request,
                                                ScanTabletResponse* response,
                                                google::protobuf::Closure* done,
                                                std::shared_ptr<quota::QuotaEntry> quota_entry) {
    return new ScanDoneWrapper(start_micros, request, response, done, quota_entry);
  }

  virtual void Run() override;

  virtual ~ScanDoneWrapper() {}

 protected:
  // Just Can Create on Heap;
  ScanDoneWrapper(int64_t start_micros, const ScanTabletRequest* request,
                  ScanTabletResponse* response, google::protobuf::Closure* done,
                  std::shared_ptr<quota::QuotaEntry> quota_entry)
      : RequestDoneWrapper(done),
        start_micros_(start_micros),
        request_(request),
        response_(response),
        quota_entry_(quota_entry) {}

  int64_t start_micros_;
  const ScanTabletRequest* request_;
  ScanTabletResponse* response_;
  std::shared_ptr<quota::QuotaEntry> quota_entry_;
};

class RemoteTabletNode : public TabletNodeServer {
 public:
  explicit RemoteTabletNode(TabletNodeImpl* tabletnode_impl);
  ~RemoteTabletNode();

  void LoadTablet(google::protobuf::RpcController* controller, const LoadTabletRequest* request,
                  LoadTabletResponse* response, google::protobuf::Closure* done);

  void UnloadTablet(google::protobuf::RpcController* controller, const UnloadTabletRequest* request,
                    UnloadTabletResponse* response, google::protobuf::Closure* done);

  void ReadTablet(google::protobuf::RpcController* controller, const ReadTabletRequest* request,
                  ReadTabletResponse* response, google::protobuf::Closure* done);

  void WriteTablet(google::protobuf::RpcController* controller, const WriteTabletRequest* request,
                   WriteTabletResponse* response, google::protobuf::Closure* done);

  void ScanTablet(google::protobuf::RpcController* controller, const ScanTabletRequest* request,
                  ScanTabletResponse* response, google::protobuf::Closure* done);

  void Query(google::protobuf::RpcController* controller, const QueryRequest* request,
             QueryResponse* response, google::protobuf::Closure* done);

  void ComputeSplitKey(google::protobuf::RpcController* controller,
                       const SplitTabletRequest* request, SplitTabletResponse* response,
                       google::protobuf::Closure* done);

  void CompactTablet(google::protobuf::RpcController* controller,
                     const CompactTabletRequest* request, CompactTabletResponse* response,
                     google::protobuf::Closure* done);

  void CmdCtrl(google::protobuf::RpcController* controller, const TsCmdCtrlRequest* request,
               TsCmdCtrlResponse* response, google::protobuf::Closure* done);

  void Update(google::protobuf::RpcController* controller, const UpdateRequest* request,
              UpdateResponse* response, google::protobuf::Closure* done);
  std::string ProfilingLog();

 private:
  void DoLoadTablet(google::protobuf::RpcController* controller, const LoadTabletRequest* request,
                    LoadTabletResponse* response, google::protobuf::Closure* done);

  void DoUnloadTablet(google::protobuf::RpcController* controller,
                      const UnloadTabletRequest* request, UnloadTabletResponse* response,
                      google::protobuf::Closure* done);

  void DoReadTablet(google::protobuf::RpcController* controller, int64_t start_micros,
                    const ReadTabletRequest* request, ReadTabletResponse* response,
                    google::protobuf::Closure* done, ReadRpcTimer* timer = NULL);

  void DoWriteTablet(google::protobuf::RpcController* controller, const WriteTabletRequest* request,
                     WriteTabletResponse* response, google::protobuf::Closure* done,
                     WriteRpcTimer* timer = NULL);

  void UpdateAuth(const QueryRequest* request, QueryResponse* response);

  void DoQuery(google::protobuf::RpcController* controller, const QueryRequest* request,
               QueryResponse* response, google::protobuf::Closure* done);

  void DoQueryTabletLoadStatus(google::protobuf::RpcController* controller,
                               const LoadTabletRequest* request, LoadTabletResponse* response,
                               google::protobuf::Closure* done);
  void DoQueryTabletUnloadStatus(google::protobuf::RpcController* controller,
                                 const UnloadTabletRequest* request, UnloadTabletResponse* response,
                                 google::protobuf::Closure* done);

  void DoScanTablet(google::protobuf::RpcController* controller, const ScanTabletRequest* request,
                    ScanTabletResponse* response, google::protobuf::Closure* done);

  void DoComputeSplitKey(google::protobuf::RpcController* controller,
                         const SplitTabletRequest* request, SplitTabletResponse* response,
                         google::protobuf::Closure* done);

  void DoMergeTablet(google::protobuf::RpcController* controller, const MergeTabletRequest* request,
                     MergeTabletResponse* response, google::protobuf::Closure* done);

  void DoCompactTablet(google::protobuf::RpcController* controller,
                       const CompactTabletRequest* request, CompactTabletResponse* response,
                       google::protobuf::Closure* done);

  void DoCmdCtrl(google::protobuf::RpcController* controller, const TsCmdCtrlRequest* request,
                 TsCmdCtrlResponse* response, google::protobuf::Closure* done);

  void DoUpdate(google::protobuf::RpcController* controller, const UpdateRequest* request,
                UpdateResponse* response, google::protobuf::Closure* done);
  void DoScheduleRpc(RpcSchedule* rpc_schedule);

  bool DoQuotaScanRpcRetry(RpcTask* rpc);
  void DoQuotaRetryScheduleRpc(RpcSchedule* rpc_schedule);

 private:
  TabletNodeImpl* tabletnode_impl_;
  // do heavyweight kinds of ctrl tasks, such as tablet load/unload
  scoped_ptr<ThreadPool> ctrl_thread_pool_;
  // do some lightweight task, such as query, reload conf, update tablet schema,
  // etc.
  scoped_ptr<ThreadPool> lightweight_ctrl_thread_pool_;
  scoped_ptr<ThreadPool> write_thread_pool_;
  scoped_ptr<ThreadPool> read_thread_pool_;
  scoped_ptr<ThreadPool> scan_thread_pool_;
  scoped_ptr<ThreadPool> compact_thread_pool_;
  scoped_ptr<RpcSchedule> read_rpc_schedule_;
  scoped_ptr<RpcSchedule> scan_rpc_schedule_;
  scoped_ptr<RpcSchedule> quota_retry_rpc_schedule_;

  enum TabletCtrlStatus {
    kCtrlWaitLoad = kTabletWaitLoad,
    kCtrlOnLoad = kTabletOnLoad,
    kCtrlWaitUnload = kTabletWaitUnload,
    kCtrlUnloading = kTabletUnloading,
  };

  std::mutex tablets_ctrl_mutex_;
  std::map<std::string, TabletCtrlStatus> tablets_ctrl_status_;

  std::unique_ptr<auth::AccessEntry> access_entry_;
  std::shared_ptr<quota::QuotaEntry> quota_entry_;
};

}  // namespace tabletnode
}  // namespace tera

#endif  // TERA_TABLETNODE_REMOTE_TABLETNODE_H_
