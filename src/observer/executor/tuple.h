// Copyright (c) 2017, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef OBSERVER_TUPLE_H_
#define OBSERVER_TUPLE_H_

#include <set>
#include <vector>
#include <boost/shared_ptr.hpp>
#include "tera.h"
#include "observer/observer.h"

namespace observer {

typedef std::set<Column> ColumnSet;

struct Tuple {
    Tuple() : t(NULL), table(NULL) {}
    ~Tuple() {
        // release Transaction
        if (t) {
            delete t;
        }
    }
    // ��������
    tera::Transaction* t;
    // Tera��
    tera::Table* table;
    // ��Key
    std::string row;
    // ���۲���
    Column observed_column;
    // ��ֵ
    std::string value;
    // ʱ���
    int64_t timestamp;
};

typedef boost::shared_ptr<Tuple> TuplePtr;
typedef std::vector<TuplePtr> Tuples;

} // namespace observer

#endif  // OBSERVER_TUPLE_H_
