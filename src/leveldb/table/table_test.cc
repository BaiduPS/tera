// Copyright (c) 2015, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/table.h"

#include <map>
#include <string>
#include <sstream>
#include <unordered_map>
#include <iomanip>
#include <atomic>
#include <thread>
#include <chrono>

#include "db/dbformat.h"
#include "db/memtable.h"
#include "db/memtable_on_leveldb.h"
#include "db/sharded_memtable.h"
#include "db/write_batch_internal.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"
#include "leveldb/table_builder.h"
#include "table/block.h"
#include "table/block_builder.h"
#include "format.h"
#include "util/random.h"
#include "util/testharness.h"
#include "util/testutil.h"
#include "leveldb/persistent_cache.h"
#include "common/event.h"
#include "util/dfs_read_thread_limiter.h"

namespace leveldb {

// Return reverse of "key".
// Used to test non-lexicographic comparators.
static std::string Reverse(const Slice& key) {
  std::string str(key.ToString());
  std::string rev("");
  for (std::string::reverse_iterator rit = str.rbegin(); rit != str.rend(); ++rit) {
    rev.push_back(*rit);
  }
  return rev;
}

namespace {
class ReverseKeyComparator : public Comparator {
 public:
  virtual const char* Name() const { return "leveldb.ReverseBytewiseComparator"; }

  virtual int Compare(const Slice& a, const Slice& b) const {
    return BytewiseComparator()->Compare(Reverse(a), Reverse(b));
  }

  virtual void FindShortestSeparator(std::string* start, const Slice& limit) const {
    std::string s = Reverse(*start);
    std::string l = Reverse(limit);
    BytewiseComparator()->FindShortestSeparator(&s, l);
    *start = Reverse(s);
  }

  virtual void FindShortSuccessor(std::string* key) const {
    std::string s = Reverse(*key);
    BytewiseComparator()->FindShortSuccessor(&s);
    *key = Reverse(s);
  }
};
}  // namespace
static ReverseKeyComparator reverse_key_comparator;

static void Increment(const Comparator* cmp, std::string* key) {
  if (cmp == BytewiseComparator()) {
    key->push_back('\0');
  } else {
    assert(cmp == &reverse_key_comparator);
    std::string rev = Reverse(*key);
    rev.push_back('\0');
    *key = Reverse(rev);
  }
}

// An STL comparator that uses a Comparator
namespace {
struct STLLessThan {
  const Comparator* cmp;

  STLLessThan() : cmp(BytewiseComparator()) {}
  STLLessThan(const Comparator* c) : cmp(c) {}
  bool operator()(const std::string& a, const std::string& b) const {
    return cmp->Compare(Slice(a), Slice(b)) < 0;
  }
};
}  // namespace

class StringSink : public WritableFile {
 public:
  ~StringSink() {}

  const std::string& contents() const { return contents_; }

  virtual Status Close() { return Status::OK(); }
  virtual Status Flush() { return Status::OK(); }
  virtual Status Sync() { return Status::OK(); }

  virtual Status Append(const Slice& data) {
    contents_.append(data.data(), data.size());
    return Status::OK();
  }

 private:
  std::string contents_;
};

class StringSource : public RandomAccessFile {
 public:
  StringSource(const Slice& contents) : contents_(contents.data(), contents.size()) {}

  virtual ~StringSource() {}

  uint64_t Size() const { return contents_.size(); }

  virtual Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const {
    if (offset > contents_.size()) {
      return Status::InvalidArgument("invalid Read offset");
    }
    if (offset + n > contents_.size()) {
      n = contents_.size() - offset;
    }
    memcpy(scratch, &contents_[offset], n);
    *result = Slice(scratch, n);
    return Status::OK();
  }

 private:
  std::string contents_;
};

typedef std::map<std::string, std::string, STLLessThan> KVMap;

// Helper class for tests to unify the interface between
// BlockBuilder/TableBuilder and Block/Table.
class Constructor {
 public:
  explicit Constructor(const Comparator* cmp) : data_(STLLessThan(cmp)) {}
  virtual ~Constructor() {}

  void Add(const std::string& key, const Slice& value) { data_[key] = value.ToString(); }

  // Finish constructing the data structure with all the keys that have
  // been added so far.  Returns the keys in sorted order in "*keys"
  // and stores the key/value pairs in "*kvmap"
  void Finish(const Options& options, std::vector<std::string>* keys, KVMap* kvmap) {
    *kvmap = data_;
    keys->clear();
    for (KVMap::const_iterator it = data_.begin(); it != data_.end(); ++it) {
      keys->push_back(it->first);
    }
    data_.clear();
    Status s = FinishImpl(options, *kvmap);
    ASSERT_TRUE(s.ok()) << s.ToString();
  }

  // Construct the data structure from the data in "data"
  virtual Status FinishImpl(const Options& options, const KVMap& data) = 0;

  virtual Iterator* NewIterator() const = 0;

  virtual const KVMap& data() { return data_; }

  virtual DB* db() const { return NULL; }  // Overridden in DBConstructor

 private:
  KVMap data_;
};

class BlockConstructor : public Constructor {
 public:
  explicit BlockConstructor(const Comparator* cmp)
      : Constructor(cmp), comparator_(cmp), block_(NULL) {}
  ~BlockConstructor() { delete block_; }
  virtual Status FinishImpl(const Options& options, const KVMap& data) {
    delete block_;
    block_ = NULL;
    BlockBuilder builder(&options);

    for (KVMap::const_iterator it = data.begin(); it != data.end(); ++it) {
      builder.Add(it->first, it->second);
    }
    // Open the block
    data_ = builder.Finish().ToString();
    BlockContents contents;
    contents.data = data_;
    contents.cachable = false;
    contents.heap_allocated = false;
    block_ = new Block(contents);
    return Status::OK();
  }
  virtual Iterator* NewIterator() const { return block_->NewIterator(comparator_); }

 private:
  const Comparator* comparator_;
  std::string data_;
  Block* block_;

  BlockConstructor();
};

template <bool enable_prefetch_scan>
class TableConstructor : public Constructor {
 public:
  TableConstructor(const Comparator* cmp)
      : Constructor(cmp), db_opt_(new Options()), source_(NULL), table_(NULL) {
    db_opt_->comparator = cmp;
  }
  ~TableConstructor() {
    Reset();
    delete db_opt_;  // cannot delete db_opt_ in `Reset()',
    db_opt_ = NULL;  // not only ~Tableconstructor() but also FinishImpl() will
                     // call Reset()
  }
  virtual Status FinishImpl(const Options& options, const KVMap& data) {
    Reset();
    StringSink sink;
    TableBuilder builder(options, &sink);

    for (KVMap::const_iterator it = data.begin(); it != data.end(); ++it) {
      builder.Add(it->first, it->second);
      ASSERT_TRUE(builder.status().ok());
    }
    Status s = builder.Finish();
    ASSERT_TRUE(s.ok()) << s.ToString();

    ASSERT_EQ(sink.contents().size(), builder.FileSize());

    // Open the table
    source_ = new StringSource(sink.contents());
    Options table_options;
    table_options.comparator = options.comparator;
    return Table::Open(table_options, source_, sink.contents().size(), &table_);
  }

  virtual Iterator* NewIterator() const {
    ReadOptions opt(db_opt_);
    opt.prefetch_scan = enable_prefetch_scan;
    return table_->NewIterator(opt);
  }

  uint64_t ApproximateOffsetOf(const Slice& key) const { return table_->ApproximateOffsetOf(key); }

 protected:
  void Reset() {
    delete table_;
    delete source_;
    table_ = NULL;
    source_ = NULL;
  }

  Options* db_opt_;
  StringSource* source_;
  Table* table_;

  TableConstructor();
};

// A helper class that converts internal format keys into user keys
class KeyConvertingIterator : public Iterator {
 public:
  explicit KeyConvertingIterator(Iterator* iter) : iter_(iter) {}
  virtual ~KeyConvertingIterator() { delete iter_; }
  virtual bool Valid() const { return iter_->Valid(); }
  virtual void Seek(const Slice& target) {
    ParsedInternalKey ikey(target, kMaxSequenceNumber, kTypeValue);
    std::string encoded;
    AppendInternalKey(&encoded, ikey);
    iter_->Seek(encoded);
  }
  virtual void SeekToFirst() { iter_->SeekToFirst(); }
  virtual void SeekToLast() { iter_->SeekToLast(); }
  virtual void Next() { iter_->Next(); }
  virtual void Prev() { iter_->Prev(); }

  virtual Slice key() const {
    assert(Valid());
    ParsedInternalKey key;
    if (!ParseInternalKey(iter_->key(), &key)) {
      status_ = Status::Corruption("malformed internal key");
      return Slice("corrupted key");
    }
    return key.user_key;
  }

  virtual Slice value() const { return iter_->value(); }
  virtual Status status() const { return status_.ok() ? iter_->status() : status_; }

 private:
  mutable Status status_;
  Iterator* iter_;

  // No copying allowed
  KeyConvertingIterator(const KeyConvertingIterator&);
  void operator=(const KeyConvertingIterator&);
};

template <class MemTableType, class... InitArgs>
static MemTable* NewMemTable(InitArgs... args) {
  return new MemTableType(std::forward<InitArgs>(args)...);
};

class MemTableConstructor : public Constructor {
 public:
  explicit MemTableConstructor(const Comparator* cmp,
                               std::function<MemTable*(InternalKeyComparator)> new_mem)
      : Constructor(cmp), internal_comparator_(cmp), new_mem_(new_mem) {
    memtable_ = new_mem_(internal_comparator_);
    memtable_->Ref();
  }
  ~MemTableConstructor() { memtable_->Unref(); }
  virtual Status FinishImpl(const Options& options, const KVMap& data) {
    memtable_->Unref();
    memtable_ = new_mem_(internal_comparator_);
    memtable_->Ref();
    int seq = 1;
    for (KVMap::const_iterator it = data.begin(); it != data.end(); ++it) {
      memtable_->Add(seq, kTypeValue, it->first, it->second);
      seq++;
    }
    return Status::OK();
  }
  virtual Iterator* NewIterator() const {
    return new KeyConvertingIterator(memtable_->NewIterator());
  }

 private:
  InternalKeyComparator internal_comparator_;
  MemTable* memtable_;
  std::function<MemTable*(InternalKeyComparator)> new_mem_;
};

class DBConstructor : public Constructor {
 public:
  explicit DBConstructor(const Comparator* cmp) : Constructor(cmp), comparator_(cmp) {
    db_ = NULL;
    NewDB();
  }
  ~DBConstructor() { delete db_; }
  virtual Status FinishImpl(const Options& options, const KVMap& data) {
    delete db_;
    db_ = NULL;
    NewDB();
    for (KVMap::const_iterator it = data.begin(); it != data.end(); ++it) {
      WriteBatch batch;
      batch.Put(it->first, it->second);
      ASSERT_TRUE(db_->Write(WriteOptions(), &batch).ok());
    }
    return Status::OK();
  }
  virtual Iterator* NewIterator() const { return db_->NewIterator(ReadOptions()); }

  virtual DB* db() const { return db_; }

 private:
  void NewDB() {
    std::string name = test::TmpDir() + "/table_testdb";

    Options options;
    options.comparator = comparator_;
    Status status = DestroyDB(name, options);
    ASSERT_TRUE(status.ok()) << status.ToString();

    options.error_if_exists = true;
    options.write_buffer_size = 10000;  // Something small to force merging
    status = DB::Open(options, name, &db_);
    ASSERT_TRUE(status.ok()) << status.ToString();
  }

  const Comparator* comparator_;
  DB* db_;
};

enum TestType {
  TABLE_TEST,
  BLOCK_TEST,
  MEMTABLE_TEST,
  DB_TEST,
  MEMTABLE_ON_LEVELDB_TEST,
  SHARD_MEMTABLE_TEST,
  SHARD_MEMTABLE_ON_LEVELDB_TEST,
  PREFETCHED_TABLE_TEST,
};

struct TestArgs {
  TestType type;
  bool reverse_compare;
  int restart_interval;
};

static const TestArgs kTestArgList[] = {
    {TABLE_TEST, false, 16},
    {TABLE_TEST, false, 1},
    {TABLE_TEST, false, 1024},
    {TABLE_TEST, true, 16},
    {TABLE_TEST, true, 1},
    {TABLE_TEST, true, 1024},
    {PREFETCHED_TABLE_TEST, false, 16},
    {PREFETCHED_TABLE_TEST, false, 1},
    {PREFETCHED_TABLE_TEST, false, 1024},
    {PREFETCHED_TABLE_TEST, true, 16},
    {PREFETCHED_TABLE_TEST, true, 1},
    {PREFETCHED_TABLE_TEST, true, 1024},

    {BLOCK_TEST, false, 16},
    {BLOCK_TEST, false, 1},
    {BLOCK_TEST, false, 1024},
    {BLOCK_TEST, true, 16},
    {BLOCK_TEST, true, 1},
    {BLOCK_TEST, true, 1024},

    // Restart interval does not matter for memtables
    {MEMTABLE_TEST, false, 16},
    {MEMTABLE_TEST, true, 16},
    {MEMTABLE_ON_LEVELDB_TEST, false, 16},
    {MEMTABLE_ON_LEVELDB_TEST, true, 16},
    {SHARD_MEMTABLE_TEST, false, 16},
    {SHARD_MEMTABLE_TEST, true, 16},
    {SHARD_MEMTABLE_ON_LEVELDB_TEST, false, 16},
    {SHARD_MEMTABLE_ON_LEVELDB_TEST, true, 16},

    // Do not bother with restart interval variations for DB
    {DB_TEST, false, 16},
    {DB_TEST, true, 16},
};
static const int kNumTestArgs = sizeof(kTestArgList) / sizeof(kTestArgList[0]);

class Harness {
 public:
  Harness() : constructor_(NULL) {}

  void Init(const TestArgs& args) {
    delete constructor_;
    constructor_ = NULL;
    options_ = Options();

    options_.block_restart_interval = args.restart_interval;
    // Use shorter block size for tests to exercise block boundary
    // conditions more.
    options_.block_size = 256;
    if (args.reverse_compare) {
      options_.comparator = &reverse_key_comparator;
    }
    switch (args.type) {
      case TABLE_TEST:
        constructor_ = new TableConstructor<false>(options_.comparator);
        break;
      case BLOCK_TEST:
        constructor_ = new BlockConstructor(options_.comparator);
        break;
      case MEMTABLE_TEST:
        constructor_ = new MemTableConstructor(
            options_.comparator,
            std::bind(
                NewMemTable<BaseMemTable, const InternalKeyComparator&, CompactStrategyFactory*>,
                std::placeholders::_1, nullptr));
        break;
      case DB_TEST:
        constructor_ = new DBConstructor(options_.comparator);
        break;
      case MEMTABLE_ON_LEVELDB_TEST:
        constructor_ = new MemTableConstructor(
            options_.comparator,
            std::bind(NewMemTable<MemTableOnLevelDB, std::string, const InternalKeyComparator&,
                                  CompactStrategyFactory*, size_t, size_t, Logger*>,
                      "MemTableTest", std::placeholders::_1, nullptr, 1024, 1024, nullptr));
        break;
      case SHARD_MEMTABLE_TEST:
        constructor_ = new MemTableConstructor(
            options_.comparator,
            std::bind(NewMemTable<ShardedMemTable, const InternalKeyComparator&,
                                  CompactStrategyFactory*, int32_t>,
                      std::placeholders::_1, nullptr, 16));
        break;
      case SHARD_MEMTABLE_ON_LEVELDB_TEST:
        constructor_ = new MemTableConstructor(
            options_.comparator,
            std::bind(NewMemTable<ShardedMemTable, std::string, InternalKeyComparator,
                                  CompactStrategyFactory*, size_t, size_t, Logger*, int32_t>,
                      "MemTableTest", std::placeholders::_1, nullptr, 1024, 1024, nullptr, 16));
        break;
      case PREFETCHED_TABLE_TEST:
        constructor_ = new TableConstructor<true>(options_.comparator);
        break;
    }
  }

  ~Harness() { delete constructor_; }

  void Add(const std::string& key, const std::string& value) { constructor_->Add(key, value); }

  void Test(Random* rnd) {
    std::vector<std::string> keys;
    KVMap data;
    constructor_->Finish(options_, &keys, &data);

    TestForwardScan(keys, data);
    TestBackwardScan(keys, data);
    TestRandomAccess(rnd, keys, data);
  }

  void TestForwardScan(const std::vector<std::string>& keys, const KVMap& data) {
    Iterator* iter = constructor_->NewIterator();
    ASSERT_TRUE(!iter->Valid());
    iter->SeekToFirst();
    for (KVMap::const_iterator model_iter = data.begin(); model_iter != data.end(); ++model_iter) {
      ASSERT_EQ(ToString(data, model_iter), ToString(iter)) << ToString(iter)
                                                            << ToString(data, model_iter);
      iter->Next();
    }
    ASSERT_TRUE(!iter->Valid());
    delete iter;
  }

  void TestBackwardScan(const std::vector<std::string>& keys, const KVMap& data) {
    Iterator* iter = constructor_->NewIterator();
    ASSERT_TRUE(!iter->Valid());
    iter->SeekToLast();
    for (KVMap::const_reverse_iterator model_iter = data.rbegin(); model_iter != data.rend();
         ++model_iter) {
      ASSERT_EQ(ToString(data, model_iter), ToString(iter));
      iter->Prev();
    }
    ASSERT_TRUE(!iter->Valid());
    delete iter;
  }

  void TestRandomAccess(Random* rnd, const std::vector<std::string>& keys, const KVMap& data) {
    static const bool kVerbose = false;
    Iterator* iter = constructor_->NewIterator();
    ASSERT_TRUE(!iter->Valid());
    KVMap::const_iterator model_iter = data.begin();
    if (kVerbose) fprintf(stderr, "---\n");
    for (int i = 0; i < 200; i++) {
      const int toss = rnd->Uniform(5);
      switch (toss) {
        case 0: {
          if (iter->Valid()) {
            if (kVerbose) fprintf(stderr, "Next\n");
            iter->Next();
            ++model_iter;
            ASSERT_EQ(ToString(data, model_iter), ToString(iter));
          }
          break;
        }

        case 1: {
          if (kVerbose) fprintf(stderr, "SeekToFirst\n");
          iter->SeekToFirst();
          model_iter = data.begin();
          ASSERT_EQ(ToString(data, model_iter), ToString(iter));
          break;
        }

        case 2: {
          std::string key = PickRandomKey(rnd, keys);
          model_iter = data.lower_bound(key);
          if (kVerbose) fprintf(stderr, "Seek '%s'\n", EscapeString(key).c_str());
          iter->Seek(Slice(key));
          ASSERT_EQ(ToString(data, model_iter), ToString(iter));
          break;
        }

        case 3: {
          if (iter->Valid()) {
            if (kVerbose) fprintf(stderr, "Prev\n");
            iter->Prev();
            if (model_iter == data.begin()) {
              model_iter = data.end();  // Wrap around to invalid value
            } else {
              --model_iter;
            }
            ASSERT_EQ(ToString(data, model_iter), ToString(iter));
          }
          break;
        }

        case 4: {
          if (kVerbose) fprintf(stderr, "SeekToLast\n");
          iter->SeekToLast();
          if (keys.empty()) {
            model_iter = data.end();
          } else {
            std::string last = data.rbegin()->first;
            model_iter = data.lower_bound(last);
          }
          ASSERT_EQ(ToString(data, model_iter), ToString(iter));
          break;
        }
      }
    }
    delete iter;
  }

  std::string ToString(const KVMap& data, const KVMap::const_iterator& it) {
    if (it == data.end()) {
      return "END";
    } else {
      return "'" + it->first + "->" + it->second + "'";
    }
  }

  std::string ToString(const KVMap& data, const KVMap::const_reverse_iterator& it) {
    if (it == data.rend()) {
      return "END";
    } else {
      return "'" + it->first + "->" + it->second + "'";
    }
  }

  std::string ToString(const Iterator* it) {
    if (!it->Valid()) {
      return "END";
    } else {
      return "'" + it->key().ToString() + "->" + it->value().ToString() + "'";
    }
  }

  std::string PickRandomKey(Random* rnd, const std::vector<std::string>& keys) {
    if (keys.empty()) {
      return "foo";
    } else {
      const int index = rnd->Uniform(keys.size());
      std::string result = keys[index];
      switch (rnd->Uniform(3)) {
        case 0:
          // Return an existing key
          break;
        case 1: {
          // Attempt to return something smaller than an existing key
          if (result.size() > 0 && result[result.size() - 1] > '\0') {
            result[result.size() - 1]--;
          }
          break;
        }
        case 2: {
          // Return something larger than an existing key
          Increment(options_.comparator, &result);
          break;
        }
      }
      return result;
    }
  }

  // Returns NULL if not running against a DB
  DB* db() const { return constructor_->db(); }

 private:
  Options options_;
  Constructor* constructor_;
};

// Test empty table/block.
TEST(Harness, Empty) {
  for (int i = 0; i < kNumTestArgs; i++) {
    Init(kTestArgList[i]);
    Random rnd(test::RandomSeed() + 1);
    Test(&rnd);
  }
}

// Special test for a block with no restart entries.  The C++ leveldb
// code never generates such blocks, but the Java version of leveldb
// seems to.
TEST(Harness, ZeroRestartPointsInBlock) {
  char data[sizeof(uint32_t)];
  memset(data, 0, sizeof(data));
  BlockContents contents;
  contents.data = Slice(data, sizeof(data));
  contents.cachable = false;
  contents.heap_allocated = false;
  Block block(contents);
  Iterator* iter = block.NewIterator(BytewiseComparator());
  iter->SeekToFirst();
  ASSERT_TRUE(!iter->Valid());
  iter->SeekToLast();
  ASSERT_TRUE(!iter->Valid());
  iter->Seek("foo");
  ASSERT_TRUE(!iter->Valid());
  delete iter;
}

// Test the empty key
TEST(Harness, SimpleEmptyKey) {
  for (int i = 0; i < kNumTestArgs; i++) {
    Init(kTestArgList[i]);
    Random rnd(test::RandomSeed() + 1);
    Add("", "v");
    Test(&rnd);
  }
}

TEST(Harness, SimpleSingle) {
  for (int i = 0; i < kNumTestArgs; i++) {
    Init(kTestArgList[i]);
    Random rnd(test::RandomSeed() + 2);
    Add("abc", "v");
    Test(&rnd);
  }
}

TEST(Harness, SimpleMulti) {
  for (int i = 0; i < kNumTestArgs; i++) {
    Init(kTestArgList[i]);
    Random rnd(test::RandomSeed() + 3);
    Add("abc", "v");
    Add("abcd", "v");
    Add("ac", "v2");
    Test(&rnd);
  }
}

TEST(Harness, SimpleSpecialKey) {
  for (int i = 0; i < kNumTestArgs; i++) {
    Init(kTestArgList[i]);
    Random rnd(test::RandomSeed() + 4);
    Add("\xff\xff", "v3");
    Test(&rnd);
  }
}

TEST(Harness, Randomized) {
  for (int i = 0; i < kNumTestArgs; i++) {
    Init(kTestArgList[i]);
    Random rnd(test::RandomSeed() + 5);
    for (int num_entries = 0; num_entries < 2000; num_entries += (num_entries < 50 ? 1 : 200)) {
      if ((num_entries % 10) == 0) {
        fprintf(stderr, "case %d of %d: num_entries = %d\n", (i + 1), int(kNumTestArgs),
                num_entries);
      }
      for (int e = 0; e < num_entries; e++) {
        std::string v;
        Add(test::RandomKey(&rnd, rnd.Skewed(4)),
            test::RandomString(&rnd, rnd.Skewed(5), &v).ToString());
      }
      Test(&rnd);
    }
  }
}

TEST(Harness, RandomizedLongDB) {
  Random rnd(test::RandomSeed());
  TestArgs args = {DB_TEST, false, 16};
  Init(args);
  int num_entries = 100000;
  for (int e = 0; e < num_entries; e++) {
    std::string v;
    Add(test::RandomKey(&rnd, rnd.Skewed(4)),
        test::RandomString(&rnd, rnd.Skewed(5), &v).ToString());
  }
  Test(&rnd);

  // We must have created enough data to force merging
  int files = 0;
  for (int level = 0; level < config::kNumLevels; level++) {
    std::string value;
    char name[100];
    snprintf(name, sizeof(name), "leveldb.num-files-at-level%d", level);
    ASSERT_TRUE(db()->GetProperty(name, &value));
    files += atoi(value.c_str());
  }
  ASSERT_GT(files, 0);
}

class MemTableTest {};

TEST(MemTableTest, Simple) {
  InternalKeyComparator cmp(BytewiseComparator());
  MemTable* memtable = new BaseMemTable(cmp, nullptr);
  memtable->Ref();
  WriteBatch batch;
  WriteBatchInternal::SetSequence(&batch, 100);
  batch.Put(std::string("k1"), std::string("v1"));
  batch.Put(std::string("k2"), std::string("v2"));
  batch.Put(std::string("k3"), std::string("v3"));
  batch.Put(std::string("largekey"), std::string("vlarge"));
  ASSERT_TRUE(WriteBatchInternal::InsertInto(&batch, memtable).ok());

  Iterator* iter = memtable->NewIterator();
  iter->SeekToFirst();
  while (iter->Valid()) {
    fprintf(stderr, "key: '%s' -> '%s'\n", iter->key().ToString().c_str(),
            iter->value().ToString().c_str());
    iter->Next();
  }

  delete iter;
  memtable->Unref();
}

TEST(MemTableTest, ShardedMemTableTest) {
  InternalKeyComparator cmp(BytewiseComparator());
  MemTable* memtable = new ShardedMemTable(cmp, nullptr, 16);
  memtable->Ref();
  WriteBatch batch;
  WriteBatchInternal::SetSequence(&batch, 100);
  for (int i = 0; i != 1000; ++i) {
    std::stringstream ss;
    ss << std::setw(4) << std::setfill('0') << i;
    batch.Put(ss.str(), ss.str());
  }
  ASSERT_TRUE(WriteBatchInternal::InsertInto(&batch, memtable).ok());

  Iterator* iter = memtable->NewIterator();
  iter->SeekToFirst();
  for (int i = 0; i != 1000; ++i) {
    ASSERT_TRUE(iter->Valid());
    std::stringstream ss;
    ss << std::setw(4) << std::setfill('0') << i;
    ASSERT_EQ(ss.str(), iter->value().ToString());
    iter->Next();
  }
  ASSERT_TRUE(!iter->Valid());
  memtable->Unref();
}

static bool Between(uint64_t val, uint64_t low, uint64_t high) {
  bool result = (val >= low) && (val <= high);
  if (!result) {
    fprintf(stderr, "Value %llu is not in range [%llu, %llu]\n", (unsigned long long)(val),
            (unsigned long long)(low), (unsigned long long)(high));
  }
  return result;
}

class TableTest {};

template <bool enable_prefetch_scan>
void ApproximateOffsetOfPlainImpl() {
  TableConstructor<enable_prefetch_scan> c(BytewiseComparator());
  c.Add("k01", "hello");
  c.Add("k02", "hello2");
  c.Add("k03", std::string(10000, 'x'));
  c.Add("k04", std::string(200000, 'x'));
  c.Add("k05", std::string(300000, 'x'));
  c.Add("k06", "hello3");
  c.Add("k07", std::string(100000, 'x'));
  std::vector<std::string> keys;
  KVMap kvmap;
  Options options;
  options.block_size = 1024;
  options.compression = kNoCompression;
  c.Finish(options, &keys, &kvmap);

  ASSERT_TRUE(Between(c.ApproximateOffsetOf("abc"), 0, 0));
  ASSERT_TRUE(Between(c.ApproximateOffsetOf("k01"), 0, 0));
  ASSERT_TRUE(Between(c.ApproximateOffsetOf("k01a"), 0, 0));
  ASSERT_TRUE(Between(c.ApproximateOffsetOf("k02"), 0, 0));
  ASSERT_TRUE(Between(c.ApproximateOffsetOf("k03"), 0, 0));
  ASSERT_TRUE(Between(c.ApproximateOffsetOf("k04"), 10000, 11000));
  ASSERT_TRUE(Between(c.ApproximateOffsetOf("k04a"), 210000, 211000));
  ASSERT_TRUE(Between(c.ApproximateOffsetOf("k05"), 210000, 211000));
  ASSERT_TRUE(Between(c.ApproximateOffsetOf("k06"), 510000, 511000));
  ASSERT_TRUE(Between(c.ApproximateOffsetOf("k07"), 510000, 511000));
  ASSERT_TRUE(Between(c.ApproximateOffsetOf("xyz"), 610000, 612000));
}

TEST(TableTest, ApproximateOffsetOfPlain) {
  ApproximateOffsetOfPlainImpl<true>();
  ApproximateOffsetOfPlainImpl<false>();
}

static bool SnappyCompressionSupported() {
  std::string out;
  Slice in = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  return port::Snappy_Compress(in.data(), in.size(), &out);
}

template <bool enable_prefetch_scan>
void ApproximateOffsetOfCompressed() {
  if (!SnappyCompressionSupported()) {
    fprintf(stderr, "skipping compression tests\n");
    return;
  }

  Random rnd(301);
  TableConstructor<enable_prefetch_scan> c(BytewiseComparator());
  std::string tmp;
  c.Add("k01", "hello");
  c.Add("k02", test::CompressibleString(&rnd, 0.25, 10000, &tmp));
  c.Add("k03", "hello3");
  c.Add("k04", test::CompressibleString(&rnd, 0.25, 10000, &tmp));
  std::vector<std::string> keys;
  KVMap kvmap;
  Options options;
  options.block_size = 1024;
  options.compression = kSnappyCompression;
  c.Finish(options, &keys, &kvmap);

  ASSERT_TRUE(Between(c.ApproximateOffsetOf("abc"), 0, 0));
  ASSERT_TRUE(Between(c.ApproximateOffsetOf("k01"), 0, 0));
  ASSERT_TRUE(Between(c.ApproximateOffsetOf("k02"), 0, 0));
  ASSERT_TRUE(Between(c.ApproximateOffsetOf("k03"), 2000, 3000));
  ASSERT_TRUE(Between(c.ApproximateOffsetOf("k04"), 2000, 3000));
  ASSERT_TRUE(Between(c.ApproximateOffsetOf("xyz"), 4000, 6000));
}

TEST(TableTest, ApproximateOffsetOfCompressed) {
  ApproximateOffsetOfCompressed<false>();
  ApproximateOffsetOfCompressed<true>();
}

class FormatTest {};

static void CheckAlign(RandomAccessFile* file, size_t alignment, uint64_t offset, size_t len) {
  DirectIOArgs args;
  char* buf = DirectIOAlign(file, offset, len, &args);
  if (buf != NULL) {
    free(buf);
  }
  ASSERT_TRUE(args.aligned_offset % alignment == 0);
  ASSERT_TRUE(args.aligned_len % alignment == 0);
  ASSERT_TRUE(args.aligned_offset >= 0 && args.aligned_offset <= offset);
  ASSERT_TRUE(args.aligned_len >= 0 && args.aligned_len >= len);
  ASSERT_TRUE(args.aligned_offset + args.aligned_len >= offset + len);
}

TEST(FormatTest, DirectIOAlign) {
  WritableFile* write_file;
  RandomAccessFile* file;
  std::string filename = "/tmp/direct_io_align";
  ASSERT_OK(Env::Default()->NewWritableFile(filename, &write_file, EnvOptions()));
  ASSERT_OK(write_file->Append("test"));
  ASSERT_OK(write_file->Close());
  ASSERT_OK(Env::Default()->NewRandomAccessFile(filename, &file, EnvOptions()));
  size_t alignment = file->GetRequiredBufferAlignment();

  uint64_t offset = 0;
  size_t len = 0;
  uint64_t offset_before = 1;
  size_t len_before = 1;
  for (int i = 0; i < 10; ++i) {
    for (int j = 0; j < 10; ++j) {
      offset = alignment * j;
      len = alignment * i;
      // same to align
      CheckAlign(file, alignment, offset, len);
      // offset = align * j + 1
      // len = align * i + 1
      CheckAlign(file, alignment, offset + offset_before, len + len_before);
      // offset = align * j - 1 && offset >= 0
      // len = align * i - 1 && len >= 0
      uint64_t tmp_offset = offset == 0 ? 0 : offset - offset_before;
      size_t tmp_len = len == 0 ? 0 : len - len_before;
      CheckAlign(file, alignment, tmp_offset, tmp_len);
      // offset = align * j + 1
      // len = align * i - 1 && len >= 0
      CheckAlign(file, alignment, offset + offset_before, tmp_len);
      // offset = align * j - 1 && offset >= 0
      // len = align * i + 1
      CheckAlign(file, alignment, tmp_offset, len + len_before);
    }
  }
}

class MockRandomAccessFile : public RandomAccessFile {
 public:
  MockRandomAccessFile(std::atomic<int>* r) : reader{r} {};

  ~MockRandomAccessFile() override = default;

  Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const override {
    ++*reader;
    event_.Wait();
    return Status::NotFound("Mock random access file");
  }

  std::string GetFileName() const override { return "abc"; }

  void Set() { event_.Set(); }

 private:
  mutable common::AutoResetEvent event_;
  std::atomic<int>* reader;
};

class MockPersistentCache : public PersistentCache {
 public:
  ~MockPersistentCache() override = default;

  Status Read(const Slice& key, size_t offset, size_t length, Slice* content,
              SstDataScratch* scratch) override {
    return Status::NotFound("not-found");
  }

  void ForceEvict(const Slice& key) override { return; }

  Status NewWriteableCacheFile(const std::string& path, WriteableCacheFile** pFile) override {
    return Status::InvalidArgument("Mock Persistent Cache");
  }

  size_t GetCapacity() const override { return 0; }

  size_t GetUsage() const override { return 0; }

  Status Open() override { return Status::InvalidArgument("Mock Persistent Cache"); }

  std::vector<std::string> GetAllKeys() override { return {}; }

  void GarbageCollect() override { return; }
};

class DfsLimiterTest {};
static void ReadFileWithCheck(RandomAccessFile* file, ReadOptions* opt,
                              std::function<bool(Status* s)> checker) {
  // handle is not important
  BlockHandle handle;
  handle.set_offset(1000);
  handle.set_size(100);
  BlockContents bc;
  auto s = ReadBlock(file, *opt, handle, &bc);
  fprintf(stderr, "status %s\n", s.ToString().c_str());
  ASSERT_TRUE(checker(&s));
}
TEST(DfsLimiterTest, RejectTest) {
  for (auto limit_val : {0, 10, 50}) {
    leveldb::DfsReadThreadLimiter::Instance().SetLimit(limit_val);
    Options opt;
    opt.persistent_cache.reset(new MockPersistentCache);
    ReadOptions read_opt;
    read_opt.db_opt = &opt;
    read_opt.enable_dfs_read_thread_limiter = true;
    std::vector<std::unique_ptr<MockRandomAccessFile>> files;
    std::atomic<int> reader{0};
    for (auto i = 0; i != limit_val + 20; ++i) {
      files.emplace_back(new MockRandomAccessFile{&reader});
    }
    std::vector<std::thread> threads;
    std::atomic<int> reject_count{0};
    for (auto i = 0; i != limit_val; ++i) {
      threads.emplace_back(ReadFileWithCheck, (RandomAccessFile*)files[i].get(), &read_opt,
                           &Status::IsNotFound);
    }
    while (reader.load() != limit_val) {
      fprintf(stderr, "Waiting for reader == %d, current %d.\n", limit_val, reader.load());
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    for (auto i = 0; i != 20; ++i) {
      threads.emplace_back(ReadFileWithCheck, (RandomAccessFile*)files[i].get(), &read_opt,
                           [&reject_count](Status* s) {
                             if (s->IsReject()) {
                               ++reject_count;
                               return true;
                             } else {
                               return false;
                             }
                           });
    }
    while (reject_count.load() != 20) {
      fprintf(stderr, "Waiting for reject_count == %d, current %d.\n", 20, reject_count.load());
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    for (auto& file : files) {
      file->Set();
    }
    for (auto& thread : threads) {
      thread.join();
    }
  }
}

TEST(TableTest, SeekToKeyGapTest) {
  StringSink sink;
  Options options;
  TableBuilder builder(options, &sink);
  builder.Add("ab", "abb");
  builder.Flush();
  builder.Add("ad", "add");
  Status s = builder.Finish();
  ASSERT_TRUE(s.ok()) << s.ToString();
  ASSERT_EQ(sink.contents().size(), builder.FileSize());
  // Open the table
  auto source = new StringSource(sink.contents());
  Table* table;
  s = Table::Open(options, source, sink.contents().size(), &table);
  ASSERT_TRUE(s.ok()) << s.ToString();
  ReadOptions r_options(&options);
  r_options.prefetch_scan = true;
  auto iter = table->NewIterator(r_options);
  iter->Seek("abb");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "ad");
  ASSERT_EQ(iter->value().ToString(), "add");
  delete iter;
}
}  // namespace leveldb

int main(int argc, char** argv) { return leveldb::test::RunAllTests(); }
