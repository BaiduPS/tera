// Copyright (c) 2015, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "proto/tabletnode_client.h"

namespace tera {
namespace tabletnode {

void TabletNodeClient::SetRpcOption(int32_t max_inflow, int32_t max_outflow,
                                    int32_t pending_buffer_size, int32_t thread_num) {
  RpcClientBase::SetOption(max_inflow, max_outflow, pending_buffer_size, thread_num);
}

TabletNodeClient::TabletNodeClient(ThreadPool* thread_pool, const std::string& server_addr,
                                   int32_t rpc_timeout)
    : RpcClient<TabletNodeServer::Stub>(server_addr),
      rpc_timeout_(rpc_timeout),
      thread_pool_(thread_pool) {}

TabletNodeClient::~TabletNodeClient() {}

bool TabletNodeClient::LoadTablet(
    const LoadTabletRequest* request, LoadTabletResponse* response,
    std::function<void(LoadTabletRequest*, LoadTabletResponse*, bool, int)> done) {
  return SendMessageWithRetry(&TabletNodeServer::Stub::LoadTablet, request, response, done,
                              "LoadTablet", rpc_timeout_, thread_pool_);
}

bool TabletNodeClient::UnloadTablet(
    const UnloadTabletRequest* request, UnloadTabletResponse* response,
    std::function<void(UnloadTabletRequest*, UnloadTabletResponse*, bool, int)> done) {
  return SendMessageWithRetry(&TabletNodeServer::Stub::UnloadTablet, request, response, done,
                              "UnloadTablet", rpc_timeout_, thread_pool_);
}

bool TabletNodeClient::ReadTablet(
    const ReadTabletRequest* request, ReadTabletResponse* response,
    std::function<void(ReadTabletRequest*, ReadTabletResponse*, bool, int)> done) {
  return SendMessageWithRetry(
      &TabletNodeServer::Stub::ReadTablet, request, response, done, "ReadTablet",
      request->has_client_timeout_ms() ? request->client_timeout_ms() : rpc_timeout_, thread_pool_);
}

bool TabletNodeClient::WriteTablet(
    const WriteTabletRequest* request, WriteTabletResponse* response,
    std::function<void(WriteTabletRequest*, WriteTabletResponse*, bool, int)> done) {
  return SendMessageWithRetry(
      &TabletNodeServer::Stub::WriteTablet, request, response, done, "WriteTablet",
      request->has_client_timeout_ms() ? request->client_timeout_ms() : rpc_timeout_, thread_pool_);
}

bool TabletNodeClient::ScanTablet(
    const ScanTabletRequest* request, ScanTabletResponse* response,
    std::function<void(ScanTabletRequest*, ScanTabletResponse*, bool, int)> done) {
  return SendMessageWithRetry(
      &TabletNodeServer::Stub::ScanTablet, request, response, done, "ScanTablet",
      request->has_timeout() ? request->timeout() : rpc_timeout_, thread_pool_);
}

bool TabletNodeClient::Query(ThreadPool* thread_pool, const QueryRequest* request,
                             QueryResponse* response,
                             std::function<void(QueryRequest*, QueryResponse*, bool, int)> done) {
  return SendMessageWithRetry(&TabletNodeServer::Stub::Query, request, response, done, "Query",
                              rpc_timeout_, thread_pool);
}

bool TabletNodeClient::ComputeSplitKey(
    const SplitTabletRequest* request, SplitTabletResponse* response,
    std::function<void(SplitTabletRequest*, SplitTabletResponse*, bool, int)> done) {
  return SendMessageWithRetry(&TabletNodeServer::Stub::ComputeSplitKey, request, response, done,
                              "ComputeSplitKey", rpc_timeout_, thread_pool_);
}

bool TabletNodeClient::CompactTablet(
    const CompactTabletRequest* request, CompactTabletResponse* response,
    std::function<void(CompactTabletRequest*, CompactTabletResponse*, bool, int)> done) {
  return SendMessageWithRetry(&TabletNodeServer::Stub::CompactTablet, request, response, done,
                              "CompactTablet", rpc_timeout_, thread_pool_);
}

bool TabletNodeClient::Update(
    const UpdateRequest* request, UpdateResponse* response,
    std::function<void(UpdateRequest*, UpdateResponse*, bool, int)> done) {
  return SendMessageWithRetry(&TabletNodeServer::Stub::Update, request, response, done, "Update",
                              rpc_timeout_, thread_pool_);
}

bool TabletNodeClient::CmdCtrl(
    const TsCmdCtrlRequest* request, TsCmdCtrlResponse* response,
    std::function<void(TsCmdCtrlRequest*, TsCmdCtrlResponse*, bool, int)> done) {
  return SendMessageWithRetry(&TabletNodeServer::Stub::CmdCtrl, request, response, done,
                              "TsCmdCtrl", rpc_timeout_, thread_pool_);
}

}  // namespace tabletnode
}  // namespace tera
