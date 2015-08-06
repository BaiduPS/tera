// Copyright (c) 2015, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Author: An Qin (qinan@baidu.com)

#ifndef TERA_TABLETNODE_TABLETNODE_ENTRY_H_
#define TERA_TABLETNODE_TABLETNODE_ENTRY_H_

#include <sofa/pbrpc/pbrpc.h>

#include "common/base/scoped_ptr.h"
#include "tera_entry.h"

namespace tera {

namespace master {
class MasterClient;
}

namespace tabletnode {

class TabletNodeImpl;
class RemoteTabletNode;

class TabletNodeEntry : public TeraEntry {
public:
    TabletNodeEntry();
    ~TabletNodeEntry();

    bool StartServer();
    bool Run();
    void ShutdownServer();

    void SetProcessorAffinity();
private:
    scoped_ptr<TabletNodeImpl> m_tabletnode_impl;
    RemoteTabletNode* m_remote_tabletnode;
    scoped_ptr<sofa::pbrpc::RpcServer> m_rpc_server;
};

} // namespace tabletnode
} // namespace tera

#endif // TERA_TABLETNODE_TABLETNODE_ENTRY_H_
