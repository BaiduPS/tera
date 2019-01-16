// Copyright (c) 2015, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/db_impl.h"

#include <iostream>
#include <algorithm>
#include <set>
#include <string>
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include <thread>

#include "db/builder.h"
#include "db/db_iter.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/memtable_on_leveldb.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "db/write_batch_internal.h"
#include "leveldb/db.h"
#include "leveldb/compact_strategy.h"
#include "leveldb/env.h"
#include "leveldb/status.h"
#include "leveldb/table.h"
#include "leveldb/table_builder.h"
#include "leveldb/table_utils.h"
#include "port/port.h"
#include "table/block.h"
#include "table/merger.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "memtable_on_leveldb.h"
#include "sharded_memtable.h"
#include "leveldb/persistent_cache.h"

namespace leveldb {

extern Status WriteStringToFileSync(Env* env, const Slice& data, const std::string& fname);

const int kNumNonTableCacheFiles = 10;

// if this file exists, ignore error in db-opening
const static std::string mark_file_name = "/__oops";

// if this file exists,
const static std::string init_load_filelock = "/__init_load_filelock";

// Information kept for every waiting writer
struct DBImpl::Writer {
  WriteBatch* batch;
  port::CondVar cv;

  explicit Writer(port::Mutex* mu) : batch(NULL), cv(mu) {}
};

struct DBImpl::CompactionState {
  Compaction* const compaction;

  // Sequence numbers < smallest_snapshot are not significant since we
  // will never have to service a snapshot below smallest_snapshot.
  // Therefore if we have seen a sequence number S <= smallest_snapshot,
  // we can drop all entries for the same key with sequence numbers < S.
  SequenceNumber smallest_snapshot;

  // Files produced by compaction
  struct Output {
    uint64_t number;
    uint64_t file_size;
    int64_t del_num;            // statistic: delete tag's percentage in sst
    std::vector<int64_t> ttls;  // use for calculate timeout percentage
    int64_t entries;
    InternalKey smallest, largest;

    Output() : number(0), file_size(0), del_num(0), entries(0) {}
  };
  std::vector<Output> outputs;

  // State kept for output being generated
  WritableFile* outfile;
  TableBuilder* builder;

  uint64_t total_bytes;
  Status status;

  Output* current_output() { return &outputs[outputs.size() - 1]; }

  explicit CompactionState(Compaction* c)
      : compaction(c),
        smallest_snapshot(kMaxSequenceNumber),
        outfile(NULL),
        builder(NULL),
        total_bytes(0) {}
};

// Fix user-supplied options to be reasonable
template <class T, class V>
static void ClipToRange(T* ptr, V minvalue, V maxvalue) {
  if (static_cast<V>(*ptr) > maxvalue) *ptr = maxvalue;
  if (static_cast<V>(*ptr) < minvalue) *ptr = minvalue;
}
Options SanitizeOptions(const std::string& dbname, const InternalKeyComparator* icmp,
                        const InternalFilterPolicy* ipolicy, const Options& src) {
  Options result = src;
  result.comparator = icmp;
  result.filter_policy = (src.filter_policy != NULL) ? ipolicy : NULL;
  ClipToRange(&result.max_open_files, 64 + kNumNonTableCacheFiles, 50000);
  ClipToRange(&result.write_buffer_size, 64 << 10, 1 << 30);
  ClipToRange(&result.block_size, 1 << 10, 4 << 20);
  if (result.info_log == NULL) {
    // Open a log file in the same directory as the db
    src.env->CreateDir(dbname);  // In case it does not exist
    Status s = src.env->NewLogger(InfoLogFileName(dbname), LogOption::LogOptionBuilder().Build(),
                                  &result.info_log);
    if (!s.ok()) {
      // No place suitable for logging
      result.info_log = NULL;
    }
  }
  if (result.block_cache == NULL) {
    result.block_cache = NewLRUCache(8 << 20);
  }

  if (result.ignore_corruption_in_open) {
    LEVELDB_LOG(result.info_log, "[%s] caution: open with ignore_corruption_in_open",
                dbname.c_str());
  }
  {
    std::string oops = dbname + mark_file_name;
    Status s = src.env->FileExists(oops);
    if (s.ok()) {
      LEVELDB_LOG(result.info_log, "[%s] caution: open with ignore_corruption_in_open",
                  dbname.c_str());
      result.ignore_corruption_in_open = true;
    }
    // Ignore error from FileExists since there is no harm
  }
  return result;
}

DBImpl::DBImpl(const Options& options, const std::string& dbname)
    : state_(kNotOpen),
      key_start_(options.key_start),
      key_end_(options.key_end),
      env_(options.env),
      internal_comparator_(options.comparator),
      internal_filter_policy_(options.filter_policy),
      options_(SanitizeOptions(dbname, &internal_comparator_, &internal_filter_policy_, options)),
      owns_info_log_(options_.info_log != options.info_log),
      owns_block_cache_(options_.block_cache != options.block_cache),
      dbname_(dbname),
      db_lock_(NULL),
      table_cache_(options_.table_cache),
      owns_table_cache_(options_.table_cache == NULL),
      shutting_down_(NULL),
      bg_cv_(&mutex_),
      writting_mem_cv_(&mutex_),
      is_writting_mem_(false),
      mem_(NewMemTable()),
      imm_(NULL),
      recover_mem_(NULL),
      logfile_(NULL),
      logfile_number_(0),
      log_(NULL),
      bound_log_size_(0),
      manual_compaction_(NULL),
      consecutive_compaction_errors_(0),
      flush_on_destroy_(false),
      need_newdb_txn_(false) {
  mem_->Ref();
  has_imm_.Release_Store(NULL);

  // Reserve ten files or so for other uses and give the rest to TableCache.
  if (owns_table_cache_) {
    LEVELDB_LOG(options_.info_log, "[%s] create new table cache.", dbname_.c_str());
    // assume 2MB per file
    const size_t table_cache_size =
        (options_.max_open_files - kNumNonTableCacheFiles) * 2LL * 1024 * 1024;  // 2MB
    table_cache_ = new TableCache(table_cache_size);
  }
  versions_ = new VersionSet(dbname_, &options_, table_cache_, &internal_comparator_);
}

bool DBImpl::ShouldForceUnloadOnError() {
  MutexLock l(&mutex_);
  return bg_error_.IsIOPermissionDenied();
}

Status DBImpl::Shutdown1() {
  assert(state_ == kOpened);
  state_ = kShutdown1;

  MutexLock l(&mutex_);
  shutting_down_.Release_Store(this);  // Any non-NULL value is ok

  LEVELDB_LOG(options_.info_log, "[%s] wait bg compact finish", dbname_.c_str());
  std::vector<CompactionTask*>::iterator it = bg_compaction_tasks_.begin();
  for (; it != bg_compaction_tasks_.end(); ++it) {
    env_->ReSchedule((*it)->id, kDumpMemTableUrgentScore, 0);
  }
  while (bg_compaction_tasks_.size() > 0) {
    bg_cv_.Wait();
  }
  // has enconutered IOPermission Denied error, return immediately and do not
  // try to compact memory table aynmore
  if (bg_error_.IsIOPermissionDenied()) {
    return bg_error_;
  }

  Status s;
  if (!options_.dump_mem_on_shutdown) {
    return s;
  }
  LEVELDB_LOG(options_.info_log, "[%s] fg compact mem table", dbname_.c_str());
  if (imm_ != NULL) {
    s = CompactMemTable();
  }
  if (s.ok()) {
    assert(imm_ == NULL);
    while (is_writting_mem_) {
      writting_mem_cv_.Wait();
    }
    imm_ = mem_;
    has_imm_.Release_Store(imm_);
    mem_ = NewMemTable();
    mem_->Ref();
    bound_log_size_ = 0;
    s = CompactMemTable();
  }
  return s;
}

Status DBImpl::Shutdown2() {
  assert(state_ == kShutdown1);
  state_ = kShutdown2;

  MutexLock l(&mutex_);
  if (bg_error_.IsIOPermissionDenied()) {
    return bg_error_;
  }
  Status s;
  if (!options_.dump_mem_on_shutdown) {
    return s;
  }
  LEVELDB_LOG(options_.info_log, "[%s] fg compact mem table", dbname_.c_str());
  assert(imm_ == NULL);
  imm_ = mem_;
  has_imm_.Release_Store(imm_);
  mem_ = NULL;
  bound_log_size_ = 0;
  return CompactMemTable();
}

DBImpl::~DBImpl() {
  if (state_ == kOpened) {
    Status s = Shutdown1();
    if (s.ok()) {
      Shutdown2();
    }
  }

  delete versions_;
  if (mem_ != NULL) mem_->Unref();
  if (imm_ != NULL) imm_->Unref();
  if (recover_mem_ != NULL) recover_mem_->Unref();
  delete log_;
  delete logfile_;
  if (owns_table_cache_) {
    delete table_cache_;
  }
  if (owns_info_log_) {
    delete options_.info_log;
  }
  if (owns_block_cache_) {
    delete options_.block_cache;
  }
  if (db_lock_) {
    env_->UnlockFile(db_lock_);
  }
}

Status DBImpl::NewDB() {
  VersionEdit new_db;
  new_db.SetComparatorName(user_comparator()->Name());
  new_db.SetLogNumber(0);
  new_db.SetNextFile(2);
  new_db.SetLastSequence(0);

  const std::string manifest = DescriptorFileName(dbname_, 1);
  WritableFile* file;
  Status s = env_->NewWritableFile(manifest, &file, EnvOptions(options_));
  if (!s.ok()) {
    return s;
  }
  {
    log::Writer log(file);
    std::string record;
    new_db.EncodeTo(&record);
    s = log.AddRecord(record);
    if (s.ok()) {
      s = file->Close();
    }
  }
  delete file;
  if (s.ok()) {
    // Make "CURRENT" file that points to the new manifest file.
    s = SetCurrentFile(env_, dbname_, 1);
  } else {
    env_->DeleteFile(manifest);
  }
  return s;
}

void DBImpl::MaybeIgnoreError(Status* s) const {
  if (s->ok() || options_.paranoid_checks) {
    // No change needed
  } else {
    LEVELDB_LOG(options_.info_log, "[%s] Ignoring error %s", dbname_.c_str(),
                s->ToString().c_str());
    *s = Status::OK();
  }
}

void DBImpl::DeleteObsoleteFiles() {
  mutex_.AssertHeld();
  if (!bg_error_.ok()) {
    // After a background error, we don't know whether a new version may
    // or may not have been committed, so we cannot safely garbage collect.
    return;
  }

  // check filesystem, and then check pending_outputs_
  std::vector<std::string> filenames;
  mutex_.Unlock();
  env_->GetChildren(dbname_, &filenames);  // Ignoring errors on purpose
  mutex_.Lock();

  // Make a set of all of the live files
  std::set<uint64_t> live = pending_outputs_;
  versions_->AddLiveFiles(&live);

  // manifest file set, keep latest 3 manifest files for backup
  // std::set<std::string> manifest_set;

  LEVELDB_LOG(options_.info_log,
              "[%s] try DeleteObsoleteFiles, total live file num: %llu,"
              " pending_outputs %lu, children_nr %lu\n",
              dbname_.c_str(), static_cast<unsigned long long>(live.size()),
              pending_outputs_.size(), filenames.size());
  uint64_t number;
  FileType type;
  for (size_t i = 0; i < filenames.size(); i++) {
    if (ParseFileName(filenames[i], &number, &type)) {
      bool keep = true;
      switch (type) {
        case kLogFile:
          keep = ((number >= versions_->LogNumber()) || (number == versions_->PrevLogNumber()));
          break;
        // case kDescriptorFile:
        //  manifest_set.insert(filenames[i]);
        //  if (manifest_set.size() > 3) {
        //      std::set<std::string>::iterator it = manifest_set.begin();
        //      ParseFileName(*it, &number, &type);
        //      if (number < versions_->ManifestFileNumber()) {
        //        // Keep my manifest file, and any newer incarnations'
        //        // (in case there is a race that allows other incarnations)
        //        filenames[i] = *it;
        //        keep = false;
        //        manifest_set.erase(it);
        //      }
        //  }
        //  break;
        case kTableFile:
          keep = (live.find(BuildFullFileNumber(dbname_, number)) != live.end());
          break;
        // case kTempFile:
        //  // Any temp files that are currently being written to must
        //  // be recorded in pending_outputs_, which is inserted into "live"
        //  keep = (live.find(number) != live.end());
        //  break;
        case kCurrentFile:
        case kDBLockFile:
        case kInfoLogFile:
          keep = true;
          break;
        case kUnknown:
        default:
          break;
      }

      if (!keep) {
        if (type == kTableFile) {
          table_cache_->Evict(dbname_, BuildFullFileNumber(dbname_, number));
          if (options_.persistent_cache) {
            auto filename = dbname_ + "/" + filenames[i];
            Slice key{filename};
            key.remove_specified_prefix(options_.dfs_storage_path_prefix);
            options_.persistent_cache->ForceEvict(key);
            LEVELDB_LOG(options_.info_log,
                        "[%s] Force evict obsolete file from persistent cache: %s\n",
                        dbname_.c_str(), filenames[i].c_str());
          }
        }
        LEVELDB_LOG(options_.info_log, "[%s] Delete type=%s #%lld, fname %s\n", dbname_.c_str(),
                    FileTypeToString(type), static_cast<unsigned long long>(number),
                    filenames[i].c_str());
        mutex_.Unlock();
        env_->DeleteFile(dbname_ + "/" + filenames[i]);
        mutex_.Lock();
      }
    }
  }
}

// Returns:
//   Status OK: iff *exists == true  -> exists
//       iff *exists == false -> not exists
//   Status not OK:
//       1). Status::Corruption -> CURRENT lost,
//       2). Status::IOError    -> Maybe request timeout, don't use *exists
Status DBImpl::ParentCurrentStatus(uint64_t parent_no, bool* exists) {
  assert(exists != NULL);
  std::string current = CurrentFileName(RealDbName(dbname_, parent_no));
  Status s = env_->FileExists(current);
  if (s.ok()) {
    *exists = true;
    return s;
  } else if (s.IsNotFound()) {
    *exists = false;
    if (options_.ignore_corruption_in_open) {
      // Drop all data in parent tablet
      LEVELDB_LOG(options_.info_log, "[%s] parent tablet(%ld) CURRENT error(drop all data): %s",
                  dbname_.c_str(), static_cast<long>(parent_no), s.ToString().c_str());
      return Status::OK();  // Data lost, reopen it as a new db
    } else {
      LEVELDB_LOG(options_.info_log, "[%s] parent tablet(%ld) CURRENT error: %s", dbname_.c_str(),
                  static_cast<long>(parent_no), s.ToString().c_str());
      return Status::Corruption(
          "CURRENT parent current lost",
          " parent tablet:" + std::to_string(static_cast<long>(parent_no)) + ", " + s.ToString());
    }
  } else {
    // Maybe request timeout, should retry open
    LEVELDB_LOG(options_.info_log, "[%s] parent tablet(%ld) CURRENT timeout", dbname_.c_str(),
                static_cast<long>(parent_no));
    return Status::IOError("parent CURRENT timeout");
  }
}

// Returns:
//   OK: iff *exists == true  -> exists
//       iff *exists == false -> not exists
//   not OK:
//       error occured, don't use *exists
Status DBImpl::DbExists(bool* exists) {
  assert(exists != NULL);
  *exists = true;
  bool current_exists = false;
  bool manifest_exists = false;
  std::vector<std::string> files;
  Status s = env_->GetChildren(dbname_, &files);
  if (!s.ok()) {
    return s;
  }
  for (size_t i = 0; i < files.size(); ++i) {
    uint64_t number;
    FileType type;
    bool valid = ParseFileName(files[i], &number, &type);
    if (!valid) {
      LEVELDB_LOG(options_.info_log, "[%s] invalid filename %s", dbname_.c_str(), files[i].c_str());
      continue;
    }
    if (type == kCurrentFile) {
      current_exists = true;
    } else if (type == kDescriptorFile) {
      manifest_exists = true;
    }
  }

  if (current_exists) {
    // db exist, ready to load, discard parent_tablets
    // don't care MANIFEST-lost in here
    options_.parent_tablets.resize(0);
    *exists = true;
    return Status::OK();
  } else {
    // CURRENT is not found
    if (manifest_exists) {
      // CURRENT file lost, but MANIFEST exist, maybe still open it
      if (options_.ignore_corruption_in_open) {
        LEVELDB_LOG(options_.info_log, "[%s] CURRENT file lost, but MANIFEST exists",
                    dbname_.c_str());
        options_.parent_tablets.resize(0);
        *exists = true;
        return Status::OK();
      } else {
        return Status::Corruption("CURRENT file lost, but manifest exists");
      }
    } else {
      // maybe
      // 1.this is a new db:
      //   normal case, don't panic
      // 2.current & manifest are all lost:
      //   we can do nothing in here, and there is no way to recover
      //   TODO(taocipian) detect this error
    }
  }

  // CURRENT & MANIFEST not exist
  if (options_.parent_tablets.size() == 0) {
    // This is a new db
    *exists = false;
    return Status::OK();
  } else if (options_.parent_tablets.size() == 1) {
    // This is a new db generated by splitting
    // We expect parent tablet exists
    return ParentCurrentStatus(options_.parent_tablets[0], exists);
  } else if (options_.parent_tablets.size() == 2) {
    // This is a new db generated by merging
    // We expect parent tablets exist
    bool parent0_exists = true;
    uint64_t parent0 = options_.parent_tablets[0];
    s = ParentCurrentStatus(options_.parent_tablets[0], &parent0_exists);
    if (!s.ok()) {
      return s;
    }

    bool parent1_exists = true;
    uint64_t parent1 = options_.parent_tablets[1];
    s = ParentCurrentStatus(options_.parent_tablets[1], &parent1_exists);
    if (!s.ok()) {
      return s;
    }

    assert((parent0_exists && parent1_exists) || options_.ignore_corruption_in_open);

    if (parent0_exists && parent1_exists) {
      *exists = true;
    } else if (parent0_exists) {
      *exists = true;
      options_.parent_tablets.resize(0);
      options_.parent_tablets.push_back(parent0);
      LEVELDB_LOG(options_.info_log, "[%s] ignore parent(%ld) lost", dbname_.c_str(), parent1);
    } else if (parent1_exists) {
      *exists = true;
      options_.parent_tablets.resize(0);
      options_.parent_tablets.push_back(parent1);
      LEVELDB_LOG(options_.info_log, "[%s] ignore parent(%ld) lost", dbname_.c_str(), parent0);
    } else {
      // Parents data lost, open this db as an empty db
      *exists = false;
      LEVELDB_LOG(options_.info_log, "[%s] ignore all parents(%ld, %ld) lost", dbname_.c_str(),
                  parent0, parent1);
    }
    return s;
  } else {
    assert(false);
  }
}

Status DBImpl::Recover(VersionEdit* edit) {
  mutex_.AssertHeld();

  {
    Status s = env_->FileExists(dbname_);
    if (s.IsNotFound()) {
      s = env_->CreateDir(dbname_);
      if (!s.ok()) {
        LEVELDB_LOG(options_.info_log, "[%s] fail to create db: %s", dbname_.c_str(),
                    s.ToString().c_str());
        return s;
      }
      need_newdb_txn_ = true;
    } else if (s.ok()) {
      // lg directory exists and not ignore curruption in open
      if (!options_.ignore_corruption_in_open) {
        s = env_->FileExists(dbname_ + init_load_filelock);
        if (s.ok()) {
          need_newdb_txn_ = true;
        } else if (!s.IsNotFound()) {
          // Unknown status
          return s;
        }
      }
    } else {
      // Unknown status
      return s;
    }
  }

  if (options_.use_file_lock) {
    Status s = env_->LockFile(LockFileName(dbname_), &db_lock_);
    if (!s.ok()) {
      return s;
    }
  }

  if (options_.ignore_corruption_in_open) {
    Status s = env_->FileExists(dbname_ + init_load_filelock);
    if (s.ok()) {
      s = env_->DeleteFile(dbname_ + init_load_filelock);
      if (!s.ok()) {
        // legacy initlock-file is dangerous
        LEVELDB_LOG(options_.info_log, "[%s] delete initlock-file failed for %s", dbname_.c_str(),
                    s.ToString().c_str());
        return Status::IOError("delete initlock-file failed");
      }
    }
    s = env_->FileExists(dbname_ + mark_file_name);
    if (s.ok()) {
      s = env_->DeleteFile(dbname_ + mark_file_name);
      if (!s.ok()) {
        // legacy mark-file is dangerous
        LEVELDB_LOG(options_.info_log, "[%s] delete mark-file failed for %s", dbname_.c_str(),
                    s.ToString().c_str());
        return Status::IOError("delete mark-file failed");
      }
    }
  }

  if (need_newdb_txn_) {
    Status s = BeginNewDbTransaction();
    if (!s.ok()) {
      return s;
    }
  }

  bool db_exists;
  Status s = DbExists(&db_exists);
  if (!s.ok()) {
    return s;
  }
  if (!db_exists) {
    s = NewDB();
    if (!s.ok()) {
      return s;
    }
  }

  LEVELDB_LOG(options_.info_log, "[%s] start VersionSet::Recover, last_seq= %llu", dbname_.c_str(),
              static_cast<unsigned long long>(versions_->LastSequence()));
  s = versions_->Recover();
  LEVELDB_LOG(options_.info_log, "[%s] end VersionSet::Recover last_seq= %llu", dbname_.c_str(),
              static_cast<unsigned long long>(versions_->LastSequence()));

  // check loss of sst files (fs exception)
  if (s.ok()) {
    std::map<uint64_t, int> expected;
    versions_->AddLiveFiles(&expected);

    // collect all tablets
    std::set<uint64_t> tablets;
    std::map<uint64_t, int>::iterator it_exp = expected.begin();
    for (; it_exp != expected.end(); ++it_exp) {
      uint64_t tablet;
      ParseFullFileNumber(it_exp->first, &tablet, NULL);
      tablets.insert(tablet);
    }

    std::set<uint64_t>::iterator it_tablet = tablets.begin();
    for (; it_tablet != tablets.end(); ++it_tablet) {
      std::string path = RealDbName(dbname_, *it_tablet);
      LEVELDB_LOG(options_.info_log, "[%s] GetChildren(%s)", dbname_.c_str(), path.c_str());
      std::vector<std::string> filenames;
      s = env_->GetChildren(path, &filenames);
      if (s.ok()) {
        // Do nothing
      } else if (s.IsTimeOut()) {
        // Should retry open
        LEVELDB_LOG(options_.info_log, "[%s] GetChildren(%s) timeout: %s", dbname_.c_str(),
                    path.c_str(), s.ToString().c_str());
        return Status::TimeOut("GetChildren timeout");
      } else {
        // Cannot read the directory
        if (options_.ignore_corruption_in_open) {
          LEVELDB_LOG(options_.info_log, "[%s] GetChildren(%s) fail: %s, still open!",
                      dbname_.c_str(), path.c_str(), s.ToString().c_str());
          // Reset the status
          s = Status::OK();
          continue;
        } else {
          LEVELDB_LOG(options_.info_log, "[%s] GetChildren(%s) fail: %s", dbname_.c_str(),
                      path.c_str(), s.ToString().c_str());
          return Status::IOError("GetChildren fail");
        }
      }
      uint64_t number;
      FileType type;
      for (size_t i = 0; i < filenames.size(); i++) {
        if (ParseFileName(filenames[i], &number, &type) && (type == kTableFile)) {
          expected.erase(BuildFullFileNumber(path, number));
        }
      }
    }
    if (!expected.empty()) {
      std::string lost_files_str = "";
      std::map<uint64_t, int>::iterator it = expected.begin();
      for (; it != expected.end(); ++it) {
        lost_files_str += FileNumberDebugString(it->first);
        if (options_.ignore_corruption_in_open) {
          edit->DeleteFile(it->second, it->first);
        }
      }
      LEVELDB_LOG(options_.info_log, "[%s] file system lost files: %s", dbname_.c_str(),
                  lost_files_str.c_str());

      if (!options_.ignore_corruption_in_open) {
        return Status::Corruption("sst lost", lost_files_str);
      }
    }
  }
  if (s.ok()) {
    state_ = kOpened;
  }
  return s;
}

Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit, Version* base, uint64_t* number) {
  mutex_.AssertHeld();
  const uint64_t start_micros = env_->NowMicros();
  FileMetaData meta;
  meta.number = BuildFullFileNumber(dbname_, versions_->NewFileNumber());
  if (number) {
    *number = meta.number;
  }
  pending_outputs_.insert(meta.number);
  Iterator* iter = mem->NewIterator();
  LEVELDB_LOG(options_.info_log, "[%s] Level-0 table #%u: started", dbname_.c_str(),
              (unsigned int)meta.number);

  uint64_t saved_size = 0;
  Status s;
  {
    uint64_t smallest_snapshot = kMaxSequenceNumber;
    if (!snapshots_.empty()) {
      smallest_snapshot = *(snapshots_.begin());
    }
    mutex_.Unlock();
    s = BuildTable(dbname_, env_, options_, table_cache_, iter, &meta, &saved_size,
                   smallest_snapshot);
    mutex_.Lock();
  }
  delete iter;
  pending_outputs_.erase(meta.number);

  // Note that if file_size is zero, the file has been deleted and
  // should not be added to the manifest.
  int level = 0;
  if (s.ok() && meta.file_size > 0) {
    const Slice min_user_key = meta.smallest.user_key();
    const Slice max_user_key = meta.largest.user_key();
    if (base != NULL && options_.drop_base_level_del_in_compaction) {
      level = base->PickLevelForMemTableOutput(min_user_key, max_user_key);
    }
    edit->AddFile(level, meta);
  }
  VersionSet::LevelSummaryStorage tmp;
  LEVELDB_LOG(options_.info_log,
              "[%s] Level-0 table #%u: dump-level %d, %lld (+ %lld ) bytes %s, %s", dbname_.c_str(),
              (unsigned int)meta.number, level, (unsigned long long)meta.file_size,
              (unsigned long long)saved_size, s.ToString().c_str(), versions_->LevelSummary(&tmp));

  CompactionStats stats;
  stats.micros = env_->NowMicros() - start_micros;
  stats.bytes_written = meta.file_size;
  stats_[level].Add(stats);
  return s;
}

// multithread safe
Status DBImpl::CompactMemTable(bool* sched_idle) {
  mutex_.AssertHeld();
  assert(imm_ != NULL);
  Status s;
  if (sched_idle) {
    *sched_idle = true;
  }
  if (imm_->BeingFlushed()) {
    return s;
  }
  imm_->SetBeingFlushed(true);

  if (imm_->ApproximateMemoryUsage() <= 0) {  // imm is empty, do nothing
    LEVELDB_LOG(options_.info_log, "[%s] CompactMemTable empty memtable %lu", dbname_.c_str(),
                GetLastSequence(false));
    imm_->Unref();
    imm_ = NULL;
    has_imm_.Release_Store(NULL);
    return s;
  }
  if (sched_idle) {
    *sched_idle = false;
  }

  // Save the contents of the memtable as a new Table
  VersionEdit edit;
  uint64_t number;
  Version* base = versions_->current();
  base->Ref();
  s = WriteLevel0Table(imm_, &edit, base, &number);
  base->Unref();

  if (s.ok() && shutting_down_.Acquire_Load()) {
    // s = Status::IOError("Deleting DB during memtable compaction");
  }

  // Replace immutable memtable with the generated Table
  if (s.ok()) {
    pending_outputs_.insert(number);  // LogAndApply donot holds lock, so use
                                      // pending_outputs_ to make sure new file
                                      // will not be deleted
    edit.SetPrevLogNumber(0);
    edit.SetLogNumber(logfile_number_);  // Earlier logs no longer needed
    if (imm_->GetLastSequence()) {
      edit.SetLastSequence(imm_->GetLastSequence());
    }
    LEVELDB_LOG(options_.info_log, "[%s] CompactMemTable SetLastSequence %lu", dbname_.c_str(),
                edit.GetLastSequence());
    s = versions_->LogAndApply(&edit, &mutex_);
    pending_outputs_.erase(number);
  }

  if (s.ok()) {
    // Commit to the new state
    imm_->Unref();
    imm_ = NULL;
    has_imm_.Release_Store(NULL);
  } else {
    // imm dump fail, reset being flush flag
    imm_->SetBeingFlushed(false);
  }

  return s;
}

void DBImpl::CompactRange(const Slice* begin, const Slice* end, int lg_no) {
  int max_level_with_files = 1;
  {
    MutexLock l(&mutex_);
    Version* base = versions_->current();
    for (int level = 1; level < config::kNumLevels; level++) {
      if (base->OverlapInLevel(level, begin, end)) {
        max_level_with_files = level;
      }
    }
  }
  TEST_CompactMemTable();  // TODO(sanjay): Skip if memtable does not overlap
  for (int level = 0; level < max_level_with_files; level++) {
    TEST_CompactRange(level, begin, end);
  }
}

void DBImpl::TEST_CompactRange(int level, const Slice* begin, const Slice* end) {
  assert(level >= 0);
  assert(level + 1 < config::kNumLevels);

  InternalKey begin_storage, end_storage;

  ManualCompaction manual;
  manual.level = level;
  manual.done = false;
  manual.being_sched = false;
  manual.compaction_conflict = kManualCompactIdle;
  if (begin == NULL) {
    manual.begin = NULL;
  } else {
    begin_storage = InternalKey(*begin, kMaxSequenceNumber, kValueTypeForSeek);
    manual.begin = &begin_storage;
  }
  if (end == NULL) {
    manual.end = NULL;
  } else {
    end_storage = InternalKey(*end, 0, static_cast<ValueType>(0));
    manual.end = &end_storage;
  }

  MutexLock l(&mutex_);
  while (!manual.done && !shutting_down_.Acquire_Load() && bg_error_.ok()) {
    if (manual_compaction_ == NULL) {  // Idle
      manual_compaction_ = &manual;
      MaybeScheduleCompaction();
    } else if (manual_compaction_->compaction_conflict == kManualCompactConflict) {
      manual_compaction_->compaction_conflict = kManualCompactIdle;
      MaybeScheduleCompaction();
    } else {  // Running either my compaction or another compaction.
      bg_cv_.Wait();
    }
  }
  if (manual_compaction_ == &manual) {
    // Cancel my manual compaction since we aborted early for some reason.
    manual_compaction_ = NULL;
  }
}

Status DBImpl::TEST_CompactMemTable() {
  // NULL batch means just wait for earlier writes to be done
  LEVELDB_LOG(options_.info_log, "[%s] CompactMemTable start", dbname_.c_str());
  Status s = Write(WriteOptions(), NULL);
  LEVELDB_LOG(options_.info_log, "[%s] CompactMemTable Write done", dbname_.c_str());
  if (s.ok()) {
    // Wait until the compaction completes
    MutexLock l(&mutex_);
    while (imm_ != NULL && bg_error_.ok()) {
      bg_cv_.Wait();
    }
    LEVELDB_LOG(options_.info_log, "[%s] CompactMemTable done", dbname_.c_str());
    if (imm_ != NULL) {
      s = bg_error_;
    }
  }
  return s;
}

// tera-specific

bool DBImpl::FindSplitKey(double ratio, std::string* split_key) {
  MutexLock l(&mutex_);
  return versions_->current()->FindSplitKey(ratio, split_key);
}

bool DBImpl::FindKeyRange(std::string* smallest_key, std::string* largest_key) {
  MutexLock l(&mutex_);
  return versions_->current()->FindKeyRange(smallest_key, largest_key);
}

bool DBImpl::MinorCompact() {
  Status s = TEST_CompactMemTable();
  return s.ok();
}

void DBImpl::AddInheritedLiveFiles(std::vector<std::set<uint64_t> >* live) {
  uint64_t tablet, lg;
  if (!ParseDbName(dbname_, NULL, &tablet, &lg)) {
    // have no tablet, return directly
    return;
  }
  assert(live && live->size() >= lg);

  std::set<uint64_t> live_all;
  {
    MutexLock l(&mutex_);
    versions_->AddLiveFiles(&live_all);
  }

  std::set<uint64_t>::iterator it;
  for (it = live_all.begin(); it != live_all.end(); ++it) {
    if (IsTableFileInherited(tablet, *it)) {
      (*live)[lg].insert(*it);
    }
  }
}

Status DBImpl::RecoverInsertMem(WriteBatch* batch, VersionEdit* edit) {
  MutexLock lock(&mutex_);

  if (recover_mem_ == NULL) {
    recover_mem_ = NewMemTable();
    recover_mem_->Ref();
  }
  uint64_t log_sequence = WriteBatchInternal::Sequence(batch);
  uint64_t last_sequence = log_sequence + WriteBatchInternal::Count(batch) - 1;

  // if duplicate record, ignore
  if (log_sequence <= recover_mem_->GetLastSequence()) {
    assert(last_sequence <= recover_mem_->GetLastSequence());
    LEVELDB_LOG(options_.info_log, "[%s] duplicate record, ignore %lu ~ %lu", dbname_.c_str(),
                log_sequence, last_sequence);
    return Status::OK();
  }

  Status status = WriteBatchInternal::InsertInto(batch, recover_mem_);
  MaybeIgnoreError(&status);
  if (!status.ok()) {
    return status;
  }
  if (recover_mem_->ApproximateMemoryUsage() > options_.write_buffer_size) {
    edit->SetLastSequence(recover_mem_->GetLastSequence());
    status = WriteLevel0Table(recover_mem_, edit, NULL);
    if (!status.ok()) {
      // Reflect errors immediately so that conditions like full
      // file-systems cause the DB::Open() to fail.
      return status;
    }
    recover_mem_->Unref();
    recover_mem_ = NULL;
  }
  return status;
}

Status DBImpl::RecoverLastDumpToLevel0(VersionEdit* edit) {
  MutexLock lock(&mutex_);
  Status s;
  if (recover_mem_ != NULL) {
    if (recover_mem_->GetLastSequence() > 0) {
      edit->SetLastSequence(recover_mem_->GetLastSequence());
      s = WriteLevel0Table(recover_mem_, edit, NULL);
    }
    recover_mem_->Unref();
    recover_mem_ = NULL;
  }
  assert(recover_mem_ == NULL);

  // LogAndApply to lg's manifest
  if (s.ok()) {
    s = versions_->LogAndApply(edit, &mutex_);
    if (s.ok()) {
      DeleteObsoleteFiles();
      MaybeScheduleCompaction();
    } else {
      LEVELDB_LOG(options_.info_log, "[%s] Fail to modify manifest", dbname_.c_str());
    }
  } else {
    LEVELDB_LOG(options_.info_log, "[%s] Fail to dump log to level 0", dbname_.c_str());
  }
  return s;
}
// end of tera-specific

bool ScoreSortGreater(std::pair<double, uint64_t> i, std::pair<double, uint64_t> j) {
  if (i.second != j.second) {
    return i.second < j.second;
  } else {
    return i.first > j.first;
  }
}
void DBImpl::MaybeScheduleCompaction() {
  mutex_.AssertHeld();
  if (shutting_down_.Acquire_Load()) {
    // DB is being deleted; no more background compactions
  } else if (bg_error_.IsIOPermissionDenied()) {
    // We have met an PermissionDenied error, not try to do compaction anymore,
    // the tablet will be unloaded soon
  } else {
    std::vector<std::pair<double, uint64_t> > scores;
    if (imm_ && !imm_->BeingFlushed()) {
      scores.emplace_back(kDumpMemTableScore, 0);
    }
    if (manual_compaction_ && !manual_compaction_->being_sched &&
        (manual_compaction_->compaction_conflict != kManualCompactConflict)) {
      scores.emplace_back(kManualCompactScore, 0);
    }
    versions_->GetCompactionScores(&scores);

    size_t qlen = std::max(scores.size(), bg_compaction_tasks_.size());
    for (size_t i = 0; i < bg_compaction_tasks_.size(); i++) {
      CompactionTask* task = bg_compaction_tasks_[i];
      scores.emplace_back(task->score, task->timeout);
    }
    std::sort(scores.begin(), scores.end(), ScoreSortGreater);

    for (size_t i = 0; i < qlen; i++) {
      if (bg_compaction_tasks_.size() < options_.max_background_compactions) {
        if (i < bg_compaction_tasks_.size()) {  // try reschedule
          CompactionTask* task = bg_compaction_tasks_[i];
          if (ScoreSortGreater(
                  scores[i], std::pair<double, uint64_t>(task->score, task->timeout))) {  // resched
            task->score = scores[i].first;
            task->timeout = scores[i].second;
            env_->ReSchedule(task->id, task->score, task->timeout);
            LEVELDB_LOG(options_.info_log,
                        "[%s] ReSchedule Compact[%ld] score= %.2f, "
                        "timeout=%lu, currency %d",
                        dbname_.c_str(), task->id, task->score, task->timeout,
                        (int)bg_compaction_tasks_.size());
            assert(scores[i].first <= 1 ||
                   scores[i].second == 0);  // if score > 1, then timeout MUST be 0
          }
        } else {  // new compact task
          CompactionTask* task = new CompactionTask;
          task->db = this;
          task->score = scores[i].first;
          task->timeout = scores[i].second;
          bg_compaction_tasks_.push_back(task);
          task->id = env_->Schedule(&DBImpl::BGWork, task, task->score, task->timeout);
          LEVELDB_LOG(options_.info_log,
                      "[%s] Schedule Compact[%ld] score= %.2f, timeout=%lu, "
                      "currency %d",
                      dbname_.c_str(), task->id, task->score, task->timeout,
                      (int)bg_compaction_tasks_.size());
          assert(scores[i].first <= 1 ||
                 scores[i].second == 0);  // if score > 1, then timeout MUST be 0
        }
      }
    }
  }
  return;
}

void DBImpl::BGWork(void* task) {
  CompactionTask* ctask = reinterpret_cast<CompactionTask*>(task);
  reinterpret_cast<DBImpl*>(ctask->db)->BackgroundCall(ctask);
}

void DBImpl::BackgroundCall(CompactionTask* task) {
  MutexLock l(&mutex_);
  LEVELDB_LOG(options_.info_log, "[%s] BackgroundCompact[%ld] score= %.2f currency %d",
              dbname_.c_str(), task->id, task->score, (int)bg_compaction_tasks_.size());
  bool sched_idle = false;
  if (!shutting_down_.Acquire_Load()) {
    Status s = BackgroundCompaction(&sched_idle);
    if (s.ok()) {
      // Success
      consecutive_compaction_errors_ = 0;
    } else if (shutting_down_.Acquire_Load()) {
      // Error most likely due to shutdown; do not wait
    } else {
      // Wait a little bit before retrying background compaction in
      // case this is an environmental problem and we do not want to
      // chew up resources for failed compactions for the duration of
      // the problem.
      bg_cv_.SignalAll();  // In case a waiter can proceed despite the error
      LEVELDB_LOG(options_.info_log,
                  "[%s] Waiting after background compaction error: %s, retry: %d", dbname_.c_str(),
                  s.ToString().c_str(), consecutive_compaction_errors_);
      ++consecutive_compaction_errors_;
      if (s.IsIOPermissionDenied() || consecutive_compaction_errors_ > 100000) {
        bg_error_ = s;
        consecutive_compaction_errors_ = 0;
      }
      mutex_.Unlock();
      int seconds_to_sleep = 1;
      for (int i = 0; i < 3 && i < consecutive_compaction_errors_ - 1; ++i) {
        seconds_to_sleep *= 2;
      }
      env_->SleepForMicroseconds(seconds_to_sleep * 1000000);
      mutex_.Lock();
    }
  } else {
    sched_idle = true;
  }

  std::vector<CompactionTask*>::iterator task_id =
      std::find(bg_compaction_tasks_.begin(), bg_compaction_tasks_.end(), task);
  assert(task_id != bg_compaction_tasks_.end());
  bg_compaction_tasks_.erase(task_id);
  delete task;

  // Previous compaction may have produced too many files in a level,
  // so reschedule another compaction if needed.
  if (!sched_idle) {
    MaybeScheduleCompaction();
  }
  bg_cv_.SignalAll();
}

Status DBImpl::BackgroundCompaction(bool* sched_idle) {
  mutex_.AssertHeld();

  *sched_idle = false;
  if (imm_ && !imm_->BeingFlushed()) {
    return CompactMemTable(sched_idle);
  }

  Status status;
  Compaction* c = NULL;
  bool is_manual = (manual_compaction_ != NULL);
  InternalKey manual_end;
  if (is_manual) {
    ManualCompaction* m = manual_compaction_;
    if (m->being_sched) {  // other thread doing manual compaction or range
                           // being compacted
      return status;
    }
    m->being_sched = true;
    bool conflict = false;
    c = versions_->CompactRange(m->level, m->begin, m->end, &conflict);
    m->compaction_conflict = conflict ? kManualCompactConflict : kManualCompactIdle;
    m->done = (c == NULL && !conflict);
    if (c != NULL) {
      manual_end = c->input(0, c->num_input_files(0) - 1)->largest;
    }
    LEVELDB_LOG(options_.info_log,
                "[%s] Manual compaction, conflit %u, at level-%d from %s .. "
                "%s; will stop at %s\n",
                dbname_.c_str(), conflict, m->level,
                (m->begin ? m->begin->DebugString().c_str() : "(begin)"),
                (m->end ? m->end->DebugString().c_str() : "(end)"),
                (m->done ? "(end)" : manual_end.DebugString().c_str()));
  } else {
    c = versions_->PickCompaction();
  }

  if (c == NULL) {
    // Nothing to do
    *sched_idle = true;
  } else if (!is_manual && c->IsTrivialMove()) {
    // Move file to next level
    assert(c->num_input_files(0) == 1);
    FileMetaData* f = c->input(0, 0);
    c->edit()->DeleteFile(c->level(), *f);
    c->edit()->AddFile(c->output_level(), *f);
    status = versions_->LogAndApply(c->edit(), &mutex_);
    VersionSet::LevelSummaryStorage tmp;
    LEVELDB_LOG(options_.info_log, "[%s] Moved #%08u, #%u to level-%d %lld bytes %s: %s\n",
                dbname_.c_str(),
                static_cast<uint32_t>(f->number >> 32 & 0x7fffffff),  // tablet number
                static_cast<uint32_t>(f->number & 0xffffffff),        // sst number
                c->output_level(), static_cast<unsigned long long>(f->file_size),
                status.ToString().c_str(), versions_->LevelSummary(&tmp));
    versions_->ReleaseCompaction(c, status);
  } else {
    status = ParallelCompaction(c);
  }
  delete c;

  if (status.ok()) {
    // Done
  } else if (shutting_down_.Acquire_Load()) {
    // Ignore compaction errors found during shutting down
  } else {
    LEVELDB_LOG(options_.info_log, "[%s] Compaction error: %s", dbname_.c_str(),
                status.ToString().c_str());
    if (bg_error_.ok()) {
      stink_bg_error_ = status;
    }
    if (options_.paranoid_checks && bg_error_.ok()) {
      bg_error_ = status;
    }
  }

  if (is_manual && manual_compaction_ != NULL) {
    ManualCompaction* m = manual_compaction_;
    m->being_sched = false;
    if (m->compaction_conflict != kManualCompactConflict) {  // PickRange success
      if (!status.ok()) {
        m->done = true;
      }
      if (!m->done) {
        // We only compacted part of the requested range.  Update *m
        // to the range that is left to be compacted.
        m->tmp_storage = manual_end;
        m->begin = &m->tmp_storage;
      }
      manual_compaction_ = NULL;
    }
  }
  return status;
}

Status DBImpl::ParallelCompaction(Compaction* c) {
  const uint64_t start_micros = env_->NowMicros();
  std::vector<Compaction*> compaction_vec;
  std::vector<CompactionState*> compaction_state_vec;
  std::vector<CompactStrategy*> compact_stragety_vec;
  assert(versions_->NumLevelFiles(c->level()) > 0);
  SequenceNumber smallest_snapshot =
      snapshots_.empty() ? kMaxSequenceNumber : *(snapshots_.begin());
  versions_->GenerateSubCompaction(c, &compaction_vec, &mutex_);
  mutex_.Unlock();

  // handle compaction without Lock
  std::vector<std::thread> thread_pool;
  thread_pool.reserve(compaction_vec.size() - 1);
  LEVELDB_LOG(options_.info_log,
              "[%s] parallel compacting %d@%d + %d@%d files, "
              "sub_compact %lu, snapshot %lu\n",
              dbname_.c_str(), c->num_input_files(0), c->level(), c->num_input_files(1),
              c->output_level(), compaction_vec.size(), smallest_snapshot);
  for (size_t i = 0; i < compaction_vec.size(); i++) {
    CompactionState* compaction = new CompactionState(compaction_vec[i]);
    assert(compaction->builder == NULL);
    assert(compaction->outfile == NULL);
    compaction->smallest_snapshot = smallest_snapshot;
    compaction_state_vec.push_back(compaction);

    CompactStrategy* compact_strategy = NewCompactStrategy(compaction);
    compact_stragety_vec.push_back(compact_strategy);
    if (i == 0) {
      LEVELDB_LOG(options_.info_log, "[%s] compact strategy: %s, snapshot %lu\n", dbname_.c_str(),
                  compact_strategy->Name(), compaction->smallest_snapshot);
    }

    if (i < compaction_vec.size() - 1) {
      thread_pool.emplace_back(&DBImpl::HandleCompactionWork, this, compaction, compact_strategy);
    } else {
      HandleCompactionWork(compaction, compact_strategy);
    }
  }
  for (auto& t : thread_pool) {
    t.join();
  }

  CompactionStats stats;
  CompactionState* compact = new CompactionState(c);
  compact->smallest_snapshot = smallest_snapshot;
  for (size_t i = 0; i < compaction_vec.size(); i++) {
    CompactionState* compaction = compaction_state_vec[i];
    for (auto& out : compaction->outputs) {
      compact->outputs.push_back(out);
      stats.bytes_written += out.file_size;
    }
    compact->total_bytes += compaction->total_bytes;
    if (compact->status.ok()) {
      compact->status = compaction->status;
    }

    CompactStrategy* compact_stragety = compact_stragety_vec[i];
    delete compact_stragety;
  }
  for (int which = 0; which < 2; which++) {
    for (int i = 0; i < compact->compaction->num_input_files(which); i++) {
      stats.bytes_read += compact->compaction->input(which, i)->file_size;
    }
  }

  mutex_.Lock();
  Status status = compact->status;
  if (status.ok()) {
    status = InstallCompactionResults(compact);
  }
  VersionSet::LevelSummaryStorage tmp;
  LEVELDB_LOG(options_.info_log, "[%s] compacted to: %s, compacte stat %s", dbname_.c_str(),
              versions_->LevelSummary(&tmp), status.ToString().c_str());
  stats.micros = env_->NowMicros() - start_micros;
  stats_[compact->compaction->output_level()].Add(stats);

  for (size_t i = 0; i < compaction_vec.size(); i++) {
    CompactionState* compaction = compaction_state_vec[i];
    CleanupCompaction(compaction);  // pop pedning output, which can be deleted
                                    // in DeleteObSoleteFiles()
    delete compaction_vec[i];
  }
  assert(compact->builder == NULL);
  assert(compact->outfile == NULL);
  CleanupCompaction(compact);

  versions_->ReleaseCompaction(c, status);  // current_version has reference to c->inputs_[0,1]
  c->ReleaseInputs();
  if (!status.IsIOPermissionDenied()) {
    DeleteObsoleteFiles();
  }
  return status;
}

void DBImpl::CleanupCompaction(CompactionState* compact) {
  mutex_.AssertHeld();
  if (compact->builder != NULL) {
    // May happen if we get a shutdown call in the middle of compaction
    compact->builder->Abandon();
    delete compact->builder;
  } else {
    assert(compact->outfile == NULL);
  }
  delete compact->outfile;
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    const CompactionState::Output& out = compact->outputs[i];
    if (pending_outputs_.erase(BuildFullFileNumber(dbname_, out.number)) > 0) {
      LEVELDB_LOG(options_.info_log, "[%s] erase pending_output #%lu", dbname_.c_str(), out.number);
    }
  }
  delete compact;
}

Status DBImpl::OpenCompactionOutputFile(CompactionState* compact) {
  assert(compact != NULL);
  assert(compact->builder == NULL);
  uint64_t file_number;
  {
    mutex_.Lock();
    file_number = versions_->NewFileNumber();
    pending_outputs_.insert(BuildFullFileNumber(dbname_, file_number));
    CompactionState::Output out;
    out.number = file_number;
    out.smallest.Clear();
    out.largest.Clear();
    compact->outputs.push_back(out);

    LEVELDB_LOG(options_.info_log, "[%s] insert pending_output #%lu", dbname_.c_str(), file_number);
    mutex_.Unlock();
  }

  // Make the output file
  std::string fname = TableFileName(dbname_, file_number);
  Status s = env_->NewWritableFile(fname, &compact->outfile, EnvOptions(options_));
  if (s.ok()) {
    compact->builder = new TableBuilder(options_, compact->outfile);
  }
  return s;
}

Status DBImpl::FinishCompactionOutputFile(CompactionState* compact, Iterator* input) {
  assert(compact != NULL);
  assert(compact->outfile != NULL);
  assert(compact->builder != NULL);

  const uint64_t output_number = compact->current_output()->number;
  assert(output_number != 0);

  // Check for iterator errors
  Status s;
  if (!options_.ignore_corruption_in_compaction) {
    s = input->status();
  }
  const uint64_t current_entries = compact->builder->NumEntries();
  compact->current_output()->entries = current_entries;
  if (s.ok()) {
    s = compact->builder->Finish();
  } else {
    compact->builder->Abandon();
  }
  const uint64_t current_bytes = compact->builder->FileSize();
  compact->current_output()->file_size = current_bytes;
  compact->total_bytes += current_bytes;
  const uint64_t saved_bytes = compact->builder->SavedSize();
  delete compact->builder;
  compact->builder = NULL;

  // Finish and check for file errors
  if (s.ok()) {
    s = compact->outfile->Close();
  }
  delete compact->outfile;
  compact->outfile = NULL;

  if (s.ok() && current_entries > 0) {
    // Verify that the table is usable
    Iterator* iter =
        table_cache_->NewIterator(ReadOptions(&options_), dbname_,
                                  BuildFullFileNumber(dbname_, output_number), current_bytes);
    s = iter->status();
    delete iter;
    if (s.ok()) {
      LEVELDB_LOG(options_.info_log, "[%s] Generated table #%llu: %lld keys, %lld (+ %lld ) bytes",
                  dbname_.c_str(), (unsigned long long)output_number,
                  (unsigned long long)current_entries, (unsigned long long)current_bytes,
                  (unsigned long long)saved_bytes);
    } else {
      LEVELDB_LOG(options_.info_log, "[%s] Verify new sst file fail #%llu", dbname_.c_str(),
                  (unsigned long long)output_number);
    }
  }
  return s;
}

Status DBImpl::InstallCompactionResults(CompactionState* compact) {
  mutex_.AssertHeld();
  LEVELDB_LOG(options_.info_log, "[%s] Compacted %d@%d + %d@%d files => %lld bytes",
              dbname_.c_str(), compact->compaction->num_input_files(0),
              compact->compaction->level(), compact->compaction->num_input_files(1),
              compact->compaction->output_level(), static_cast<long long>(compact->total_bytes));

  // Add compaction outputs, skip file without entries
  compact->compaction->AddInputDeletions(compact->compaction->edit());
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    CompactionState::Output& out = compact->outputs[i];
    if (out.entries <= 0) {
      continue;
    }

    std::sort(out.ttls.begin(), out.ttls.end());
    uint32_t idx = out.ttls.size() * options_.ttl_percentage / 100;
    compact->compaction->edit()->AddFile(
        compact->compaction->output_level(), BuildFullFileNumber(dbname_, out.number),
        out.file_size, out.smallest, out.largest,
        out.del_num * 100 / out.entries /* delete tag percentage */,
        ((out.ttls.size() > 0) && (idx < out.ttls.size())) ? out.ttls[idx]
                                                           : 0 /* sst's check ttl's time */,
        ((out.ttls.size() > 0) && (idx < out.ttls.size())) ? idx * 100 / out.ttls.size()
                                                           : 0 /* delete tag percentage */);
    LEVELDB_LOG(
        options_.info_log,
        "[%s] AddFile, level %d, number #%lu, entries %ld, del_nr %lu"
        ", ttl_nr %lu, del_p %lu, ttl_check_ts %lu, ttl_p %lu\n",
        dbname_.c_str(), compact->compaction->output_level(), out.number, out.entries, out.del_num,
        out.ttls.size(), out.del_num * 100 / out.entries,
        ((out.ttls.size() > 0) && (idx < out.ttls.size())) ? out.ttls[idx] : 0,
        ((out.ttls.size() > 0) && (idx < out.ttls.size())) ? idx * 100 / out.ttls.size() : 0);
  }
  return versions_->LogAndApply(compact->compaction->edit(), &mutex_);
}

CompactStrategy* DBImpl::NewCompactStrategy(CompactionState* compact) {
  CompactStrategy* compact_strategy = NULL;
  if (options_.compact_strategy_factory) {
    compact_strategy = options_.compact_strategy_factory->NewInstance();
    compact_strategy->SetSnapshot(compact->smallest_snapshot);
  }
  return compact_strategy;
}

// ** Handle sub compaction without LOCK **
void DBImpl::HandleCompactionWork(CompactionState* compact, CompactStrategy* compact_strategy) {
  Compaction* c = compact->compaction;
  Status& status = compact->status;
  Iterator* input = versions_->MakeInputIterator(c);
  if (c->sub_compact_start_ == "") {
    input->SeekToFirst();
  } else {
    input->Seek(c->sub_compact_start_);
  }
  Slice end_key(c->sub_compact_end_);
  LEVELDB_LOG(options_.info_log, "[%s] handle %d@%d + %d@%d compact, range [%s, %s)\n",
              dbname_.c_str(), c->num_input_files(0), c->level(), c->num_input_files(1),
              c->output_level(), c->sub_compact_start_.c_str(), c->sub_compact_end_.c_str());

  ParsedInternalKey ikey;
  std::string current_user_key;
  bool has_current_user_key = false;
  SequenceNumber last_sequence_for_key = kMaxSequenceNumber;
  for (; input->Valid() && !shutting_down_.Acquire_Load();) {
    // Prioritize immutable compaction work
    if (has_imm_.NoBarrier_Load() != NULL) {
      mutex_.Lock();
      if (imm_ && !imm_->BeingFlushed()) {
        CompactMemTable();   // no need check failure, because imm_ not null if
                             // dump fail.
        bg_cv_.SignalAll();  // Wakeup MakeRoomForWrite() if necessary
      }
      mutex_.Unlock();
    }

    Slice key = input->key();
    if (end_key.size() > 0 &&
        internal_comparator_.InternalKeyComparator::Compare(input->key(), end_key) >= 0) {
      LEVELDB_LOG(options_.info_log, "[%s] handle %d@%d + %d@%d compact, stop at %s\n",
                  dbname_.c_str(), c->num_input_files(0), c->level(), c->num_input_files(1),
                  c->output_level(), end_key.data());
      break;  // reach end_key, stop this sub compaction
    }

    if (compact->compaction->ShouldStopBefore(key) &&
        compact->builder != NULL) {  // should not overlap level() + 2 too much
      status = FinishCompactionOutputFile(compact, input);
      if (!status.ok()) {
        break;
      }
    }

    // Handle key/value, add to state, etc.
    bool drop = false;
    if (!ParseInternalKey(key, &ikey)) {
      // Do not hide error keys
      current_user_key.clear();
      has_current_user_key = false;
      last_sequence_for_key = kMaxSequenceNumber;
    } else {
      if (!has_current_user_key ||
          user_comparator()->Compare(ikey.user_key, Slice(current_user_key)) != 0) {
        // First occurrence of this user key
        current_user_key.assign(ikey.user_key.data(), ikey.user_key.size());
        has_current_user_key = true;
        last_sequence_for_key = kMaxSequenceNumber;
      }

      if (RollbackDrop(ikey.sequence, rollbacks_)) {
        drop = true;
      } else if (last_sequence_for_key <= compact->smallest_snapshot &&
                 last_sequence_for_key != kMaxSequenceNumber) {
        // Hidden by an newer entry for same user key
        drop = true;  // (A)
      } else if (ikey.type == kTypeDeletion && ikey.sequence <= compact->smallest_snapshot &&
                 options_.drop_base_level_del_in_compaction &&
                 compact->compaction->IsBaseLevelForKey(ikey.user_key)) {
        // For this user key:
        // (1) there is no data in higher levels
        // (2) data in lower levels will have larger sequence numbers
        // (3) data in layers that are being compacted here and have
        //     smaller sequence numbers will be dropped in the next
        //     few iterations of this loop (by rule (A) above).
        // Therefore this deletion marker is obsolete and can be dropped.
        drop = true;
      } else if (compact_strategy && ikey.sequence <= compact->smallest_snapshot) {
        std::string lower_bound;
        if (options_.drop_base_level_del_in_compaction) {
          lower_bound = compact->compaction->drop_lower_bound();
        }
        drop = compact_strategy->Drop(ikey.user_key, ikey.sequence, lower_bound);
      }

      last_sequence_for_key = ikey.sequence;
    }
#if 0
    LEVELDB_LOG(options_.info_log,
        "  Compact: %s, seq %d, type: %d %d, drop: %d, is_base: %d, "
        "%d smallest_snapshot: %d",
        ikey.user_key.ToString().c_str(),
        (int)ikey.sequence, ikey.type, kTypeValue, drop,
        compact->compaction->IsBaseLevelForKey(ikey.user_key),
        (int)last_sequence_for_key, (int)compact->smallest_snapshot);
#endif

    bool has_atom_merged = false;
    if (!drop) {
      // Open output file if necessary
      if (compact->builder == NULL) {
        status = OpenCompactionOutputFile(compact);
        if (!status.ok()) {
          break;
        }
      }
      assert(compact->builder);
      if (compact->builder->NumEntries() == 0) {
        compact->current_output()->smallest.DecodeFrom(key);
      }
      compact->current_output()->largest.DecodeFrom(key);

      if (compact_strategy && ikey.sequence <= compact->smallest_snapshot) {
        std::string merged_value;
        std::string merged_key;
        has_atom_merged = compact_strategy->MergeAtomicOPs(input, &merged_value, &merged_key);
        if (has_atom_merged) {
          Slice newValue(merged_value);
          compact->builder->Add(Slice(merged_key), newValue);
        }
      }

      if (!has_atom_merged) {
        // check del tag and ttl tag
        bool del_tag = false;
        int64_t ttl = -1;
        compact_strategy && compact_strategy->CheckTag(ikey.user_key, &del_tag, &ttl);
        if (ikey.type == kTypeDeletion || del_tag) {
          compact->current_output()->del_num++;
        } else if (ttl > 0) {  // del tag has not ttl
          compact->current_output()->ttls.push_back(ttl);
        }
        compact->builder->Add(key, input->value());
      }
      // Close output file if it is big enough
      if (compact->builder->FileSize() >= compact->compaction->MaxOutputFileSize()) {
        status = FinishCompactionOutputFile(compact, input);
        if (!status.ok()) {
          break;
        }
      }
    }

    if (!has_atom_merged) {
      input->Next();
    }
  }

  if (status.ok() && shutting_down_.Acquire_Load()) {
    status = Status::IOError("Deleting DB during compaction");
  }
  if (status.ok() && compact->builder != NULL) {
    status = FinishCompactionOutputFile(compact, input);
  }
  if (status.ok() && !input->status().ok()) {
    if (options_.ignore_corruption_in_compaction) {
      LEVELDB_LOG(options_.info_log, "[%s] ignore compaction error: %s", dbname_.c_str(),
                  input->status().ToString().c_str());
    } else {
      status = input->status();
    }
  }
  delete input;
  input = NULL;
}

struct IterState {
  port::Mutex* mu;
  Version* version;
  MemTable* mem;
  MemTable* imm;
};

static void CleanupIteratorState(void* arg1, void* arg2) {
  IterState* state = reinterpret_cast<IterState*>(arg1);
  state->mu->Lock();
  state->mem->Unref();
  if (state->imm != NULL) state->imm->Unref();
  state->version->Unref();
  state->mu->Unlock();
  delete state;
}

Iterator* DBImpl::NewInternalIterator(const ReadOptions& options, SequenceNumber* latest_snapshot) {
  IterState* cleanup = new IterState;
  mutex_.Lock();
  *latest_snapshot = GetLastSequence(false);

  MemTable* mem = mem_;
  MemTable* imm = imm_;
  Version* current = versions_->current();
  mem->Ref();
  if (imm != NULL) imm->Ref();
  current->Ref();
  mutex_.Unlock();

  // Collect together all needed child iterators
  std::vector<Iterator*> child_iterators;
  child_iterators.reserve(current->NumFiles(0) + config::kNumLevels);
  child_iterators.emplace_back(mem->NewIterator());
  if (imm != NULL) {
    child_iterators.emplace_back(imm->NewIterator());
  }
  current->AddIterators(options, &child_iterators);
  Iterator* internal_iter =
      NewMergingIterator(&internal_comparator_, &child_iterators[0], child_iterators.size());

  cleanup->mu = &mutex_;
  cleanup->mem = mem;
  cleanup->imm = imm;
  cleanup->version = current;
  internal_iter->RegisterCleanup(CleanupIteratorState, cleanup, NULL);

  return internal_iter;
}

Iterator* DBImpl::TEST_NewInternalIterator() {
  SequenceNumber ignored;
  return NewInternalIterator(ReadOptions(), &ignored);
}

int64_t DBImpl::TEST_MaxNextLevelOverlappingBytes() {
  MutexLock l(&mutex_);
  return versions_->MaxNextLevelOverlappingBytes();
}

Status DBImpl::Get(const ReadOptions& options, const Slice& key, std::string* value) {
  Status s;
  MutexLock l(&mutex_);
  SequenceNumber snapshot;
  if (options.snapshot != kMaxSequenceNumber) {
    snapshot = options.snapshot;
  } else {
    snapshot = GetLastSequence(false);
  }

  MemTable* mem = mem_;
  MemTable* imm = imm_;
  Version* current = versions_->current();
  mem->Ref();
  if (imm != NULL) imm->Ref();
  current->Ref();

  bool have_stat_update = false;
  Version::GetStats stats;

  // Unlock while reading from files and memtables
  {
    mutex_.Unlock();
    // First look in the memtable, then in the immutable memtable (if any).
    LookupKey lkey(key, snapshot);
    if (mem->Get(lkey, value, options.rollbacks, &s)) {
      // Done
    } else if (imm != NULL && imm->Get(lkey, value, options.rollbacks, &s)) {
      // Done
    } else {
      s = current->Get(options, lkey, value, &stats);
      have_stat_update = true;
    }
    mutex_.Lock();
  }

  if (have_stat_update && current->UpdateStats(stats)) {
    MaybeScheduleCompaction();
  }
  mem->Unref();
  if (imm != NULL) imm->Unref();
  current->Unref();
  return s;
}

Iterator* DBImpl::NewIterator(const ReadOptions& options) {
  SequenceNumber latest_snapshot;
  Iterator* internal_iter = NewInternalIterator(options, &latest_snapshot);
  return NewDBIterator(
      &dbname_, env_, user_comparator(), internal_iter,
      (options.snapshot != kMaxSequenceNumber ? options.snapshot : latest_snapshot),
      options.rollbacks);
}

const uint64_t DBImpl::GetSnapshot(uint64_t last_sequence) {
  MutexLock l(&mutex_);
  if (options_.use_memtable_on_leveldb) {
    if (mem_) {
      mem_->GetSnapshot(last_sequence);
    }
    if (imm_) {
      imm_->GetSnapshot(last_sequence);
    }
  }
  snapshots_.insert(last_sequence);
  return last_sequence;
}

void DBImpl::ReleaseSnapshot(uint64_t sequence_number) {
  MutexLock l(&mutex_);
  if (options_.use_memtable_on_leveldb) {
    if (mem_) {
      mem_->ReleaseSnapshot(sequence_number);
    }
    if (imm_) {
      imm_->ReleaseSnapshot(sequence_number);
    }
  }

  std::multiset<uint64_t>::iterator it = snapshots_.find(sequence_number);
  assert(it != snapshots_.end());
  snapshots_.erase(it);
}

const uint64_t DBImpl::Rollback(uint64_t snapshot_seq, uint64_t rollback_point) {
  MutexLock l(&mutex_);
  assert(rollback_point >= snapshot_seq);
  rollbacks_[snapshot_seq] = rollback_point;
  return rollback_point;
}

// Convenience methods
Status DBImpl::Put(const WriteOptions& o, const Slice& key, const Slice& val) {
  return DB::Put(o, key, val);
}

Status DBImpl::Delete(const WriteOptions& options, const Slice& key) {
  return DB::Delete(options, key);
}

bool DBImpl::BusyWrite() {
  MutexLock l(&mutex_);
  return (versions_->NumLevelFiles(0) >= options_.l0_slowdown_writes_trigger);
}

void DBImpl::Workload(double* write_workload) {
  MutexLock l(&mutex_);
  std::vector<std::pair<double, uint64_t> > scores;
  versions_->GetCompactionScores(&scores);
  double wwl = scores.size() > 0 ? scores[0].first : 0;
  if (wwl >= 0) {
    *write_workload = wwl;
  } else {
    *write_workload = 0;
  }
}

Status DBImpl::Write(const WriteOptions& options, WriteBatch* my_batch) {
  Writer w(&mutex_);
  w.batch = my_batch;

  MutexLock l(&mutex_);
  writers_.push_back(&w);
  while (&w != writers_.front()) {
    w.cv.Wait();
  }

  // May temporarily unlock and wait.
  Status status = MakeRoomForWrite(my_batch == NULL);

  if (status.ok() && my_batch != NULL) {  // NULL batch is for compactions
    uint64_t batch_sequence = WriteBatchInternal::Sequence(my_batch);
    WriteBatch* updates = my_batch;

    // Apply to memtable.  We can release the lock
    // during this phase since &w is currently responsible for logging
    // and protects against concurrent loggers and concurrent writes
    // into mem_.
    is_writting_mem_ = true;

    mutex_.Unlock();
    status = WriteBatchInternal::InsertInto(updates, mem_);
    mutex_.Lock();

    if (WriteBatchInternal::Count(updates) > 0) {
      mem_->SetNonEmpty();
    }
    if (mem_->Empty() && imm_ == NULL) {
      versions_->SetLastSequence(batch_sequence - 1);
    }
  }

  assert(writers_.front() == &w);
  writers_.pop_front();

  // Notify new head of write queue
  if (!writers_.empty()) {
    writers_.front()->cv.Signal();
  }

  is_writting_mem_ = false;
  writting_mem_cv_.Signal();
  return status;
}

// REQUIRES: mutex_ is held
// REQUIRES: this thread is currently at the front of the writer queue
Status DBImpl::MakeRoomForWrite(bool force) {
  mutex_.AssertHeld();
  Status s;
  assert(!writers_.empty());
  bool allow_delay = !force;
  while (true) {
    if (!bg_error_.ok()) {
      // Yield previous error
      s = bg_error_;
      break;
    } else if (allow_delay && versions_->NumLevelFiles(0) >= config::kL0_SlowdownWritesTrigger) {
      // We are getting close to hitting a hard limit on the number of
      // L0 files.  Rather than delaying a single write by several
      // seconds when we hit the hard limit, start delaying each
      // individual write by 1ms to reduce latency variance.  Also,
      // this delay hands over some CPU to the compaction thread in
      // case it is sharing the same core as the writer.
      mutex_.Unlock();
      env_->SleepForMicroseconds(1000);
      allow_delay = false;  // Do not delay a single write more than once
      mutex_.Lock();
    } else if (shutting_down_.Acquire_Load()) {
      break;
    } else if (!force && (mem_->ApproximateMemoryUsage() <= options_.write_buffer_size)) {
      // There is room in current memtable
      break;
    } else if (imm_ != NULL) {
      // We have filled up the current memtable, but the previous
      // one is still being compacted, so we wait.
      LEVELDB_LOG(options_.info_log, "[%s] Current memtable full; waiting...\n", dbname_.c_str());
      bg_cv_.Wait();
    } else if (versions_->NumLevelFiles(0) >= config::kL0_StopWritesTrigger) {
      // There are too many level-0 files.
      LEVELDB_LOG(options_.info_log, "[%s] Too many L0 files; waiting...\n", dbname_.c_str());
      bg_cv_.Wait();
    } else {
      imm_ = mem_;
      has_imm_.Release_Store(imm_);
      mem_ = NewMemTable();
      mem_->Ref();
      bound_log_size_ = 0;
      force = false;  // Do not force another compaction if have room
      MaybeScheduleCompaction();
    }
  }
  return s;
}

void DBImpl::AddBoundLogSize(uint64_t size) {
  {
    MutexLock lock(&mutex_);
    if (mem_->Empty()) {
      return;
    }
    bound_log_size_ += size;
    if (bound_log_size_ < options_.flush_triggered_log_size) {
      return;
    }
    if (imm_ != NULL) {
      LEVELDB_LOG(options_.info_log, "[%s] [TimeoutCompaction] imm_ != NULL", dbname_.c_str());
      return;
    }
  }
  Status s = Write(WriteOptions(), NULL);
  if (s.ok()) {
    LEVELDB_LOG(options_.info_log, "[%s] [TimeoutCompaction] done %lu", dbname_.c_str(), size);
  } else {
    LEVELDB_LOG(options_.info_log, "[%s] [TimeoutCompaction] fail", dbname_.c_str());
  }
}

bool DBImpl::GetProperty(const Slice& property, std::string* value) {
  value->clear();

  MutexLock l(&mutex_);
  Slice in = property;
  Slice prefix("leveldb.");
  if (!in.starts_with(prefix)) return false;
  in.remove_prefix(prefix.size());

  if (in.starts_with("num-files-at-level")) {
    in.remove_prefix(strlen("num-files-at-level"));
    uint64_t level;
    bool ok = ConsumeDecimalNumber(&in, &level) && in.empty();
    if (!ok || static_cast<int>(level) >= config::kNumLevels) {
      return false;
    } else {
      char buf[100];
      snprintf(buf, sizeof(buf), "%d", versions_->NumLevelFiles(static_cast<int>(level)));
      *value = buf;
      return true;
    }
  } else if (in == "stats") {
    char buf[200];
    snprintf(buf, sizeof(buf),
             "                               Compactions\n"
             "Level  Files Size(MB) Time(sec) Read(MB) Write(MB)\n"
             "--------------------------------------------------\n");
    value->append(buf);
    for (int level = 0; level < config::kNumLevels; level++) {
      int files = versions_->NumLevelFiles(level);
      if (stats_[level].micros > 0 || files > 0) {
        snprintf(buf, sizeof(buf), "%3d %8d %8.0f %9.0f %8.0f %9.0f\n", level, files,
                 versions_->NumLevelBytes(level) / 1048576.0, stats_[level].micros / 1e6,
                 stats_[level].bytes_read / 1048576.0, stats_[level].bytes_written / 1048576.0);
        value->append(buf);
      }
    }
    return true;
  } else if (in == "sstables") {
    *value = versions_->current()->DebugString();
    return true;
  } else if (in == "verify-db-integrity") {
    std::map<uint64_t, uint64_t> check_file_list;
    std::map<std::string, std::string> manifest_error_list;
    versions_->AddLiveFilesWithSize(&check_file_list);
    l.Unlock();

    std::set<uint64_t> tablet_num;
    std::map<uint64_t, uint64_t>::iterator it = check_file_list.begin();
    for (; it != check_file_list.end(); ++it) {
      uint64_t tablet;
      ParseFullFileNumber(it->first, &tablet, NULL);
      tablet_num.insert(tablet);
    }

    Status s;
    std::set<uint64_t>::iterator it_tablet = tablet_num.begin();
    for (; s.ok() && it_tablet != tablet_num.end(); ++it_tablet) {
      std::vector<std::string> filenames;
      std::string tablet_path = RealDbName(dbname_, *it_tablet);
      // early return during shutdown
      if (shutting_down_.Acquire_Load()) {
        return false;
      }
      s = env_->GetChildren(tablet_path, &filenames);

      uint64_t number;
      FileType type;
      for (size_t i = 0; i < filenames.size(); i++) {
        // early return during shutdown
        if (shutting_down_.Acquire_Load()) {
          return false;
        }
        if (ParseFileName(filenames[i], &number, &type) && (type == kTableFile)) {
          uint64_t tablet_no = BuildFullFileNumber(tablet_path, number);
          if (check_file_list.find(tablet_no) == check_file_list.end()) {
            continue;
          }

          uint64_t fsize = 0;
          Status s1 = env_->GetFileSize(tablet_path + "/" + filenames[i], &fsize);
          // when some one timeout, maybe dfs master is busy now,
          // return immediate, check after next round
          if (s1.IsTimeOut()) {
            return false;
          }
          if (!s1.ok() || check_file_list[tablet_no] == fsize) {
            check_file_list.erase(tablet_no);
          } else {
            LEVELDB_LOG(options_.info_log,
                        "[%s] verify db, size mismatch, "
                        "path %s, tablet %s, size(in meta) %lu, size(in fs) %lu",
                        dbname_.c_str(), tablet_path.c_str(), filenames[i].c_str(),
                        check_file_list[tablet_no], fsize);
          }
        } else if (ParseFileName(filenames[i], &number, &type) && (type == kCurrentFile)) {
          std::string desc_name;
          Status s2 = ReadFileToString(env_, tablet_path + "/" + filenames[i], &desc_name);
          if (s2.ok()) {
            if (!desc_name.empty() && desc_name[desc_name.size() - 1] == '\n') {
              desc_name.resize(desc_name.size() - 1);
            }
            s2 = env_->FileExists(tablet_path + "/" + desc_name);
            if (s2.IsNotFound() || desc_name.empty()) {
              manifest_error_list[tablet_path] = desc_name;
              LEVELDB_LOG(options_.info_log,
                          "[%s] verify db, cur mani mismatch, "
                          "tablet %s, manifest %s is miss",
                          dbname_.c_str(), tablet_path.c_str(), desc_name.c_str());
            }
          }
        }
      }
    }

    l.Lock();
    std::map<uint64_t, uint64_t> live;
    versions_->AddLiveFilesWithSize(&live);

    it = check_file_list.begin();
    while (it != check_file_list.end()) {
      if (live.find(it->first) == live.end()) {
        it = check_file_list.erase(it);
      } else {
        ++it;
      }
    }

    if (s.ok() && check_file_list.empty() && manifest_error_list.empty()) {  // verify success
      value->append("verify_success");
    } else if (s.ok() && !manifest_error_list.empty()) {  // current manifest mismatch
      value->append("manifest_error");
      LEVELDB_LOG(options_.info_log, "[%s] db_manifest_error", dbname_.c_str());
    } else if (s.ok()) {  // sst file lost
      value->append("verify_fail");
      LEVELDB_LOG(options_.info_log, "[%s] db_corruption, lost %lu", dbname_.c_str(),
                  check_file_list.size());
    }
    return s.ok();
  } else if (in == "compaction_error") {
    if (!bg_error_.ok()) {
      stink_bg_error_ = bg_error_;
    }

    if (!stink_bg_error_.ok()) {
      value->append("Corruption: ");
      value->append(stink_bg_error_.ToString());
    }
    bool ret = !stink_bg_error_.ok();
    // reset stink_bg_error_ to ok
    stink_bg_error_ = Status::OK();
    return ret;
  }

  return false;
}

void DBImpl::GetApproximateSizes(const Range* range, int n, uint64_t* sizes) {
  // TODO(opt): better implementation
  Version* v;
  {
    MutexLock l(&mutex_);
    versions_->current()->Ref();
    v = versions_->current();
  }

  for (int i = 0; i < n; i++) {
    // Convert user_key into a corresponding internal key.
    InternalKey k1(range[i].start, kMaxSequenceNumber, kValueTypeForSeek);
    InternalKey k2(range[i].limit, kMaxSequenceNumber, kValueTypeForSeek);
    uint64_t start = versions_->ApproximateOffsetOf(v, k1);
    uint64_t limit = versions_->ApproximateOffsetOf(v, k2);
    sizes[i] = (limit >= start ? limit - start : 0);
  }

  {
    MutexLock l(&mutex_);
    v->Unref();
  }
}

void DBImpl::GetApproximateSizes(uint64_t* size, std::vector<uint64_t>* lgsize,
                                 uint64_t* mem_table_size) {
  MutexLock l(&mutex_);
  versions_->current()->GetApproximateSizes(size);

  if (mem_table_size) {
    *mem_table_size = 0;
  }
  // add mem&imm size
  if (size) {
    if (mem_) {
      auto tmp_mem_size = mem_->ApproximateMemoryUsage();
      *size += tmp_mem_size;
      if (mem_table_size) {
        *mem_table_size += tmp_mem_size;
      }
    }
    if (imm_) {
      auto tmp_imm_table_size = imm_->ApproximateMemoryUsage();
      *size += tmp_imm_table_size;
      if (mem_table_size) {
        *mem_table_size += tmp_imm_table_size;
      }
    }
  }
}

uint64_t DBImpl::GetLastSequence(bool is_locked) {
  if (is_locked) {
    mutex_.Lock();
  }
  uint64_t retval;
  if (mem_->GetLastSequence() > 0) {
    retval = mem_->GetLastSequence();
  } else if (imm_ != NULL && imm_->GetLastSequence()) {
    retval = imm_->GetLastSequence();
  } else {
    retval = versions_->LastSequence();
  }
  if (is_locked) {
    mutex_.Unlock();
  }
  return retval;
}

MemTable* DBImpl::NewMemTable() const {
  if (!options_.use_memtable_on_leveldb) {
    if (options_.memtable_shard_num > 1) {
      LEVELDB_LOG(options_.info_log, "[%s] New shard base memTable, shard num: %d", dbname_.c_str(),
                  options_.memtable_shard_num);
      return new ShardedMemTable(
          internal_comparator_,
          options_.enable_strategy_when_get ? options_.compact_strategy_factory : NULL,
          options_.memtable_shard_num);
    } else {
      return new BaseMemTable(internal_comparator_, options_.enable_strategy_when_get
                                                        ? options_.compact_strategy_factory
                                                        : NULL);
    }
  } else {
    Logger* info_log = NULL;
    MemTable* new_mem = nullptr;
    if (options_.memtable_shard_num > 1) {
      LEVELDB_LOG(options_.info_log, "[%s] New shard leveldb memTable, shard num: %d",
                  dbname_.c_str(), options_.memtable_shard_num);
      new_mem = new ShardedMemTable(
          dbname_, internal_comparator_, options_.compact_strategy_factory,
          options_.memtable_ldb_write_buffer_size, options_.memtable_ldb_block_size, info_log,
          options_.memtable_shard_num);
    } else {
      // Logger* info_log = options_.info_log;
      new_mem = new MemTableOnLevelDB(
          dbname_, internal_comparator_, options_.compact_strategy_factory,
          options_.memtable_ldb_write_buffer_size, options_.memtable_ldb_block_size, info_log);
    }

    for (auto snapshot : snapshots_) {
      new_mem->GetSnapshot(snapshot);
    }

    return new_mem;
  }
}

uint64_t DBImpl::GetLastVerSequence() {
  MutexLock l(&mutex_);
  return versions_->LastSequence();
}

Iterator* DBImpl::NewInternalIterator() {
  SequenceNumber ignored;
  return NewInternalIterator(ReadOptions(), &ignored);
}

Status DBImpl::BeginNewDbTransaction() {
  LEVELDB_LOG(options_.info_log, "[%s] Begin load txn", dbname_.c_str());
  std::string lock_file_name = dbname_ + init_load_filelock;
  Status s = env_->FileExists(lock_file_name);
  if (s.IsNotFound()) {
    // first new by split or merge add __lock file for first create lg
    s = WriteStringToFileSync(env_, "\n", lock_file_name);
    if (!s.ok()) {
      LEVELDB_LOG(options_.info_log, "[%s] fail to start new db transaction: %s", dbname_.c_str(),
                  s.ToString().c_str());
      return s;
    }
  } else if (s.ok()) {
    // have failed before this time to open
    // && ignore corruption option not opened
    // && don't have sst files
    // need to delete all files in this db except __init_load_filelock file
    LEVELDB_LOG(options_.info_log, "[%s] begin to re-new db: %s", dbname_.c_str(),
                s.ToString().c_str());
    std::vector<std::string> files;
    s = env_->GetChildren(dbname_, &files);
    if (!s.ok()) {
      LEVELDB_LOG(options_.info_log, "[%s] fail to re-new db: %s", dbname_.c_str(),
                  s.ToString().c_str());
      return s;
    }
    uint64_t number;
    FileType type;
    for (size_t f = 0; f < files.size(); ++f) {
      if (ParseFileName(files[f], &number, &type) && kTableFile == type) {
        return s;
      }
    }
    for (size_t f = 0; f < files.size(); ++f) {
      std::string old_file_name = dbname_ + "/" + files[f];
      if ("/" + files[f] != init_load_filelock) {
        s = env_->DeleteFile(old_file_name);
        if (!s.ok()) {
          LEVELDB_LOG(options_.info_log, "[%s] fail to re-new db: %s", dbname_.c_str(),
                      s.ToString().c_str());
          return s;
        }
      }
    }
  }
  return s;
}

Status DBImpl::CommitNewDbTransaction() {
  Status s;
  if (!need_newdb_txn_) {
    return s;
  }

  LEVELDB_LOG(options_.info_log, "[%s] Commit load txn", dbname_.c_str());
  std::string lock_file_name = dbname_ + init_load_filelock;
  s = env_->FileExists(lock_file_name);
  if (s.IsNotFound()) {
    // lost lock file during this new db
    LEVELDB_LOG(options_.info_log, "[%s] find transaction lock file fail: %s", dbname_.c_str(),
                s.ToString().c_str());
    return Status::Corruption("newdb transaction lock disappeared");
  } else if (s.ok()) {
    s = env_->DeleteFile(lock_file_name);
    if (!s.ok()) {
      LEVELDB_LOG(options_.info_log, "[%s] delete transaction lock file fail: %s", dbname_.c_str(),
                  s.ToString().c_str());
      return Status::Corruption("newdb transaction clean lock faild");
    }
  }
  return s;
}

void DBImpl::GetCurrentLevelSize(std::vector<int64_t>* result) {
  MutexLock l(&mutex_);
  versions_->GetCurrentLevelSize(result);
}

}  // namespace leveldb
