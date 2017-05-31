// Copyright (c) 2017, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef OBSERVER_OBSERVER_H_
#define OBSERVER_OBSERVER_H_

#include <string>
#include <vector>
#include <map>
#include "tera.h"

#pragma GCC visibility push(default)
namespace observer {

struct Column {
    std::string table_name;
    std::string family;
    std::string qualifier;  
    bool operator<(const Column& other) const {
        std::string str1 = table_name + family + qualifier;
        std::string str2 = other.table_name + other.family + other.qualifier;
        return str1 < str2;
    }
};

typedef std::vector<Column> ColumnList;
// <TableName, ColumnList>
typedef std::map<std::string, ColumnList> ColumnMap;

/// ����Tera��������, ʵ�ִ��ģ���������ʵʱ����������
class Observer {
public:
    // ����۲���Ψһ��ʾ���Լ����۲���
    Observer(const std::string& observer_name, ColumnList& observed_columns);
    virtual ~Observer();

    // �û�ʵ�ִ˽ӿ��õ��۲����ϱ仯����, ��ɼ���
    virtual bool OnNotify(tera::Transaction* t, 
            tera::Table* table, 
            const std::string& row, 
            const Column& column, 
            const std::string& value, 
            int64_t timestamp);

    // �û�ʵ�ִ˽ӿ�����ʼ������
    virtual bool Init();
    
    // �û�ʵ�ִ˽ӿ�����������
    virtual bool Close();

    // ���֪ͨ
    bool Ack(ColumnList& columns, const std::string& row, int64_t timestamp);

    // ����֪ͨ, ��������observer
    bool Notify(ColumnList& columns, const std::string& row, int64_t timestamp);

    std::string GetName() const;
    ColumnMap& GetColumnMap();
private:
    Observer(const Observer&);
    void operator=(const Observer&);

private:
    std::string observer_name_;
    ColumnMap column_map_;
};

} // namespace observer
#pragma GCC visibility pop

#endif  // ONSERVER_OBSERVER_H_
