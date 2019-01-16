// Copyright (c) 2015, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TERA_LOAD_BALANCER_LB_IMPL_H_
#define TERA_LOAD_BALANCER_LB_IMPL_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/mutex.h"
#include "common/thread_pool.h"
#include "load_balancer/balancer.h"
#include "load_balancer/lb_node.h"
#include "load_balancer/options.h"
#include "load_balancer/plan.h"
#include "master/tablet_manager.h"
#include "master/tabletnode_manager.h"
#include "proto/load_balancer_rpc.pb.h"
#include "sdk/client_impl.h"

namespace tera {
namespace load_balancer {

class LBImpl {
 public:
  LBImpl();
  virtual ~LBImpl();

  bool Init();

  void CmdCtrl(const CmdCtrlRequest* request, CmdCtrlResponse* response,
               google::protobuf::Closure* done);

 private:
  void ScheduleMetaBalance();
  void ScheduleUnityBalance();
  void DoMetaBalance();
  void DoUnityBalance();
  bool BlanceClusterByTable(
      const std::shared_ptr<Balancer>& balancer,
      std::map<std::string, std::vector<std::shared_ptr<LBTabletNode>>>& nodes_by_table,
      std::vector<Plan>* plans);

  void InitOptions();
  bool CreateLBInput(const std::vector<tera::master::TablePtr>& tables,
                     const std::vector<tera::master::TabletNodePtr>& nodes,
                     const std::vector<tera::master::TabletPtr>& tablets,
                     std::vector<std::shared_ptr<LBTabletNode>>* lb_nodes);
  bool CreateLBInputByTable(
      const std::vector<tera::master::TablePtr>& tables,
      const std::vector<tera::master::TabletNodePtr>& nodes,
      const std::vector<tera::master::TabletPtr>& tablets,
      std::map<std::string, std::vector<std::shared_ptr<LBTabletNode>>>* nodes_by_table);
  bool Collect(std::vector<tera::master::TabletNodePtr>* nodes,
               std::vector<tera::master::TablePtr>* tables,
               std::vector<tera::master::TabletPtr>* tablets);
  bool CollectNodes(std::vector<tera::master::TabletNodePtr>* nodes);
  bool NodeInfoToNode(const TabletNodeInfo& info, tera::master::TabletNodePtr node);
  tera::master::NodeState StringToNodeState(const std::string& str);
  bool CollectTablets(std::vector<tera::master::TablePtr>* tables,
                      std::vector<tera::master::TabletPtr>* tablets);

  bool IsSafemode() const;
  bool SetSafemode(bool value);
  void SafeModeCmdCtrl(const CmdCtrlRequest* request, CmdCtrlResponse* response);

  bool GetMasterSafemode(bool* safe_mode);
  void ExecutePlan(const std::vector<Plan>& plans);

  std::string GetMetaNodeAddr() const;
  bool SetMetaNodeAddr(const std::string& addr);

  void DebugCollect(const std::vector<tera::master::TabletNodePtr>& nodes,
                    const std::vector<tera::master::TablePtr>& tables,
                    const std::vector<tera::master::TabletPtr>& tablets);
  void DebugLBNode(const std::vector<std::shared_ptr<LBTabletNode>>& lb_nodes);
  void DebugLBNodeByTable(
      const std::map<std::string, std::vector<std::shared_ptr<LBTabletNode>>>& nodes_by_table);
  void DebugPlan(const std::vector<Plan>& plans);

 private:
  mutable Mutex mutex_;

  std::unique_ptr<ThreadPool> thread_pool_;
  std::unique_ptr<tera::Client> sdk_client_;
  LBOptions lb_options_;

  bool safemode_;
  std::string meta_node_addr_;

  bool lb_debug_mode_;
};

}  // namespace load_balancer
}  // namespace tera

#endif  // TERA_LOAD_BALANCER_LB_IMPL_H_
