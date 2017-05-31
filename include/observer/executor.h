// Copyright (c) 2017, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef OBSERVER_EXECUTOR_H_
#define OBSERVER_EXECUTOR_H_

#include "observer.h"

#pragma GCC visibility push(default)
namespace observer {

/// ִ����
class Executor {
public:
    static Executor* NewExecutor();
    
    // ע����Ҫ���е�Observer
    virtual bool RegisterObserver(Observer* observer) = 0;
    
    // �����ӿ�
    virtual bool Run() = 0;

    Executor() {}
    virtual ~Executor() {}

private:
    Executor(const Executor&);
    void operator=(const Executor&);
};

} // namespace observer
#pragma GCC visibility pop

#endif  // OBSERVER_EXECUTOR_H_
