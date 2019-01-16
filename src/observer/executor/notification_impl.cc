// Copyright (c) 2015-2017, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "observer/executor/notification_impl.h"

#include <glog/logging.h>

#include "common/timer.h"
#include "common/base/string_number.h"
#include "sdk/global_txn_internal.h"
#include "types.h"

namespace tera {
namespace observer {

Notification* GetNotification(const std::shared_ptr<NotifyCell>& notify_cell) {
  return new NotificationImpl(notify_cell);
}

NotificationImpl::NotificationImpl(const std::shared_ptr<NotifyCell>& notify_cell)
    : notify_cell_(notify_cell),
      start_timestamp_(get_micros()),
      notify_timestamp_(0),
      ack_callback_(nullptr),
      notify_callback_(nullptr),
      ack_context_(nullptr),
      notify_context_(nullptr) {}

void NotificationImpl::SetAckCallBack(Notification::Callback callback) {
  if (notify_cell_->notify_transaction == NULL) {
    ack_callback_ = callback;
  } else {
    LOG(ERROR) << "Support ack callback only when TransactionType = kNoneTransaction";
    abort();
  }
}

void NotificationImpl::SetAckContext(void* context) { ack_context_ = context; }

void* NotificationImpl::GetAckContext() { return ack_context_; }

void NotificationImpl::Ack(Table* t, const std::string& row_key, const std::string& column_family,
                           const std::string& qualifier) {
  if (notify_cell_->notify_transaction != NULL) {
    notify_cell_->notify_transaction->Ack(t, row_key, column_family, qualifier);
    return;
  }

  // kNoneTransaction
  tera::RowMutation* mutation = t->NewRowMutation(row_key);
  std::string notify_qulifier = PackNotifyName(column_family, qualifier);
  mutation->DeleteColumns(kNotifyColumnFamily, notify_qulifier, start_timestamp_);
  if (ack_callback_ != nullptr) {
    mutation->SetContext(this);
    mutation->SetCallBack([](RowMutation* mu) {
      NotificationImpl* notification_impl = (NotificationImpl*)mu->GetContext();
      ErrorCode err = mu->GetError();
      notification_impl->ack_callback_(notification_impl, err);
      delete mu;
    });
  }
  t->ApplyMutation(mutation);
  if (ack_callback_ == nullptr) {
    delete mutation;
  }
}

void NotificationImpl::SetNotifyCallBack(Notification::Callback callback) {
  if (notify_cell_->notify_transaction == NULL) {
    notify_callback_ = callback;
  } else {
    LOG(ERROR) << "Support notify callback only when TransactionType = kNoneTransaction";
    abort();
  }
}

void NotificationImpl::SetNotifyContext(void* context) { notify_context_ = context; }

void* NotificationImpl::GetNotifyContext() { return notify_context_; }

void NotificationImpl::Notify(Table* t, const std::string& row_key,
                              const std::string& column_family, const std::string& qualifier) {
  if (notify_cell_->notify_transaction != NULL) {
    notify_cell_->notify_transaction->Notify(t, row_key, column_family, qualifier);
    return;
  }

  // kNoneTransaction
  if (notify_timestamp_ == 0) {
    notify_timestamp_ = get_micros();
  }

  std::string notify_qulifier = PackNotifyName(column_family, qualifier);
  tera::RowMutation* mutation = t->NewRowMutation(row_key);

  mutation->Put(kNotifyColumnFamily, notify_qulifier, NumberToString(notify_timestamp_),
                notify_timestamp_);

  if (notify_callback_ != nullptr) {
    mutation->SetContext(this);
    mutation->SetCallBack([](RowMutation* mu) {
      NotificationImpl* notification_impl = (NotificationImpl*)mu->GetContext();
      ErrorCode err = mu->GetError();
      notification_impl->notify_callback_(notification_impl, err);
      delete mu;
    });
  }
  t->ApplyMutation(mutation);
  if (notify_callback_ == nullptr) {
    delete mutation;
  }
}

void NotificationImpl::Done() { delete this; }

}  // namespace observer
}  // namespace tera
