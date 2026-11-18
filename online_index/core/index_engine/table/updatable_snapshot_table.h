#pragma once
#include <fcntl.h>
#include <sys/stat.h>

#include <cstdint>
#include <limits>
#include <ostream>
#include <string>

#include "butil/containers/flat_map.h"
#include "butil/fd_guard.h"
#include "butil/file_util.h"
#include "butil/object_pool.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"
#include "index_engine/table/recycle.h"
#include "index_engine/util/allocator.h"
#include "index_engine/util/epoch_controller.h"
#include "index_engine/util/logging.h"
#include "index_engine/util/table_util.h"
#include "proto/header.pb.h"

DECLARE_int32(retire_value_batch_size);

namespace index_engine {

template <typename V>
class IndexAndDataReader {
 public:
  struct Node {
    uint64_t key;
    union {
      uint64_t offset;
      V* value;
    } opaque;
  };

  bool ReadIndexAndData(
      const DirBased& parent_dir,
      const ShardMeta& meta,
      std::string& flatbuffers_data,
      std::unique_ptr<Node[]>& index_buffer
  ) {
    DirBased dir(parent_dir.FilePath(meta.dir_name()));
    TableHeader header;
    butil::Timer timer(butil::Timer::STARTED);
    if (ReadIndexFile_(dir, meta.index_file_name(), header, meta, index_buffer) < 0) {
      PLOG(ERROR) << name_ << " read index file error, file: " << dir.dir() << "/" << meta.index_file_name();
      return false;
    }

    if (!dir.ReadFile(meta.data_file_name(), flatbuffers_data)) {
      LITE_LOG(ERROR) << name_ << " read data file error, data_file: " << meta.data_file_name();
      return false;
    }
    timer.stop();
    LITE_LOG(INFO) << name_ << " meta: " << ProtoStringify(meta) << " load index cost: " << timer.m_elapsed() << " ms";

    return true;
  }

  void set_name(const std::string& name) {
    name_ = name;
  }

  uint32_t record_count() const {
    return record_count_;
  }

  uint64_t bucket_size() const {
    return bucket_size_;
  }

 private:
  int ReadIndexFile_(
      const DirBased& dir,
      const std::string& index_file,
      TableHeader& header,
      const ShardMeta& meta,
      std::unique_ptr<Node[]>& index_buffer
  ) {
    int64_t index_total_size;
    butil::fd_guard index_fd(dir.OpenForRead(index_file, index_total_size));
    if (index_fd < 0) {
      return -1;
    }

    google::protobuf::io::FileInputStream raw_input(index_fd);
    google::protobuf::io::CodedInputStream input(&raw_input);

    uint32_t header_size = 0;
    if (!input.ReadRaw(&header_size, sizeof(header_size))) {
      LITE_LOG(ERROR) << name_ << " Read header_size error from file:" << index_file;
      return -1;
    }

    std::string header_buffer;
    if (!input.ReadString(&header_buffer, (int) header_size)) {
      LITE_LOG(ERROR) << name_ << " Read header_buffer error from file:" << index_file;
      return -1;
    }

    if (!header.ParseFromString(header_buffer)) {
      PLOG(ERROR) << "fail to load header";
      return -1;
    }

    if (header.version() != 0) {
      LITE_LOG(ERROR) << name_ << " Invalid version: " << header.version() << " file:" << index_file;
      return -1;
    }

    bucket_size_ = header.bucket_count();
    if ((bucket_size_ & (bucket_size_ - 1)) != 0) {
      LITE_LOG(ERROR) << name_ << " bucket size must be power of 2";
      return -1;
    }

    if (header.item_count() == 0) {
      record_count_ = meta.items_total_count();
    } else {
      record_count_ = header.item_count();
    }

    if (bucket_size_ < record_count_) {
      bucket_size_ = butil::find_power2(record_count_);
    }

    size_t index_data_size = index_total_size - input.CurrentPosition();
    LITE_LOG(INFO) << name_ << " load header: " << ProtoStringify(header) << " index_size:" << index_total_size
                   << " CurrentPosition:" << input.CurrentPosition() << " data_size:" << index_data_size
                   << " record_count: " << record_count_ << " bucket_size: " << bucket_size_
                   << " record_percent: " << GetPercent(record_count_, bucket_size_);

    if (::lseek(index_fd, (long) (sizeof(header_size) + header_size), SEEK_SET) < 0) {
      LITE_LOG(ERROR) << name_ << " lseek error";
      return -1;
    }

    if (index_data_size < record_count_ * sizeof(Node)) {
      LITE_LOG(WARNING) << name_ << " Invalid index data size:" << index_data_size << " record:" << record_count_
                        << " sizeof(Node):" << sizeof(Node);
      return -1;
    }

    index_buffer.reset(new Node[record_count_]);
    if (!dir.ReadFromFD(index_fd, reinterpret_cast<char*>(&index_buffer[0]), record_count_ * sizeof(Node))) {
      LITE_LOG(WARNING) << name_ << " read index file error, file: " << index_file
                        << " size:" << record_count_ * sizeof(Node);
      return -1;
    }

    // ignore footer...
    return 0;
  }

  std::string name_;
  uint32_t record_count_ = 0;
  uint64_t bucket_size_ = 0;
};

template <typename V, typename N, typename Allocator = ChunkAllocator<V>>
class SnapshotTableBase {
 private:
  DEFINE_TYPE_NAME(N);

 public:
  using IndexAndDataReader = index_engine::IndexAndDataReader<V>;
  using Node = typename IndexAndDataReader::Node;
  using allocator_type = Allocator;

  SnapshotTableBase() = default;
  virtual ~SnapshotTableBase() = default;
  explicit SnapshotTableBase(base::PolymorphismEpochController* epoch)
      : epoch_(epoch) {
    index_and_data_reader_.set_name(typename_);
  }

  void set_epoch(base::PolymorphismEpochController* epoch) {
    epoch_ = epoch;
  }
  allocator_type* allocator() {
    return allocator_;
  };
  void set_allocator(allocator_type* allocator) {
    allocator_ = allocator;
  }

  uint32_t Size() const {
    return record_count_;
  }

  bool Load(const DirBased& parent_dir, const ShardMeta& meta) {
    LITE_LOG(INFO) << typename_ << " start to load "
                   << " meta: " << ProtoStringify(meta) << " parent_dir: " << parent_dir;
    std::string flatbuffers_data;
    if (!this->ReadIndexAndData_(parent_dir, meta, flatbuffers_data)) {
      return false;
    }

    butil::Timer timer{butil::Timer::STARTED};
    if (BuildIndex_(flatbuffers_data.data()) != 0) {
      LITE_LOG(ERROR) << typename_ << " build index failed";
      return false;
    }
    timer.stop();
    LITE_LOG(INFO) << typename_ << " meta: " << ProtoStringify(meta) << " build index cost: " << timer.m_elapsed()
                   << " ms";
    this->inited_ = true;
    return true;
  }

  void ForEach(const std::function<void(uint64_t, V*)>& visitor) {
    Visit(visitor);
  }

  virtual void Visit(const std::function<void(uint64_t, V*)>& visitor) {
    for (uint32_t i = 0; i < record_count_; ++i) {
      auto* node = &index_buffer_[i];
      auto* value = node->opaque.value;
      visitor(node->key, value);
    }
  }

 protected:
  virtual int BuildIndex_(const char* data_buffer) = 0;
  static_assert(sizeof(Node) == 16, "Node size MUST be 16 byte");
  bool ReadIndexAndData_(const DirBased& parent_dir, const ShardMeta& meta, std::string& flatbuffers_data) {
    if (!index_and_data_reader_.ReadIndexAndData(parent_dir, meta, flatbuffers_data, index_buffer_)) {
      LITE_LOG(WARNING) << typename_ << " fail to read index and data";
      return false;
    }
    this->bucket_size_ = index_and_data_reader_.bucket_size();
    this->record_count_ = index_and_data_reader_.record_count();

    return true;
  }

  int ReleaseAll_() {
    int released_count = 0;
    this->Visit([this, &released_count](uint64_t, V* value) {
      released_count++;
      this->allocator_->Release(value);
    });
    return released_count;
  }

  IndexAndDataReader index_and_data_reader_;
  allocator_type* allocator_{nullptr};
  base::PolymorphismEpochController* epoch_;
  std::unique_ptr<Node[]> index_buffer_;
  uint32_t record_count_ = 0;
  std::unique_ptr<Node*[]> buckets_;
  uint64_t bucket_size_ = 0;
  bool inited_{false};
};

template <typename Format>
class ImmutableSnapshotTable : public SnapshotTableBase<
                                   typename Format::struct_type,
                                   ImmutableSnapshotTable<Format>,
                                   DefaultAllocator<typename Format::struct_type>> {
 private:
  DEFINE_TYPE_NAME(ImmutableSnapshotTable<Format>);

 public:
  using struct_type = typename Format::struct_type;
  using flatbuffers_type = typename Format::flatbuffers_type;
  using SnapshotBase = SnapshotTableBase<struct_type, ImmutableSnapshotTable<Format>, DefaultAllocator<struct_type>>;
  using Node = typename SnapshotBase::Node;

  ImmutableSnapshotTable() = default;
  explicit ImmutableSnapshotTable(base::PolymorphismEpochController* epoch)
      : SnapshotBase(epoch) {}
  ~ImmutableSnapshotTable() {
    int released_count = this->ReleaseAll_();
    LITE_LOG(INFO) << " destruction of " << typename_ << " addr: " << this << " release " << released_count
                   << " object";
  }

  const struct_type* Seek(uint64_t key) {
    uint32_t bucket_index = key & (this->bucket_size_ - 1);

    auto* node = this->buckets_[bucket_index];
    auto* end = this->buckets_[bucket_index + 1];

    while (node < end) {
      if (node->key == key) {
        return node->opaque.value;
      }
      node += 1;
    }
    return nullptr;
  }

 protected:
  int BuildIndex_(const char* data_buffer) override {
    if (this->bucket_size_ == 0) {
      this->bucket_size_ = 1;
      this->buckets_.reset(new Node*[this->bucket_size_ + 1]());
      return 0;
    }

    this->buckets_.reset(new Node*[this->bucket_size_ + 1]);
    butil::Timer timer{butil::Timer::STARTED};
    int32_t last_bucket_index = -1;
    std::vector<struct_type*> elements;
    elements.resize(this->record_count_);
    this->allocator_->MutexAcquireBatch(elements, this->record_count_);
    timer.stop();
    LITE_LOG(INFO) << typename_ << " Allocate " << this->record_count_ << " elements"
                   << " cost " << timer.m_elapsed() << " ms"
                   << " elements: " << elements.size();

    for (size_t i = 0; i < this->record_count_; ++i) {
      auto* node = &this->index_buffer_[i];
      uint64_t offset = node->opaque.offset;
      auto* struct_ptr = elements[i];

      Format::StructFromFlatbuffers(struct_ptr, reinterpret_cast<const flatbuffers_type*>(&data_buffer[offset]));
      node->opaque.value = struct_ptr;

      auto bucket_index = (int32_t) (node->key % this->bucket_size_);
      if (bucket_index != last_bucket_index) {
        for (int32_t idx = last_bucket_index + 1; idx <= bucket_index; idx++) {
          this->buckets_[idx] = &this->index_buffer_[i];
        }
        last_bucket_index = bucket_index;
      }
    }

    for (int32_t idx = last_bucket_index + 1; idx <= (int32_t) this->bucket_size_; idx++) {
      this->buckets_[idx] = &this->index_buffer_[this->record_count_];
    }

    LITE_LOG(INFO) << typename_ << " Load complete records:" << this->record_count_
                   << " buckets:" << this->bucket_size_;
    return 0;
  }
};

template <class Format>
class UpdatableSnapshotTable
    : public SnapshotTableBase<VersionedValue<typename Format::struct_type>, UpdatableSnapshotTable<Format>> {
 private:
  DEFINE_TYPE_NAME(UpdatableSnapshotTable<Format>);

 public:
  using struct_type = typename Format::struct_type;
  using flatbuffers_type = typename Format::flatbuffers_type;
  using versioned_value_type = VersionedValue<struct_type>;
  using ValueType = typename Format::ValueType;
  using SnapshotBase = SnapshotTableBase<versioned_value_type, UpdatableSnapshotTable<Format>>;
  using Node = typename SnapshotBase::Node;
  using Allocator = typename SnapshotBase::allocator_type;

  using GarbageCollector = BatchValueGarbageCollector<versioned_value_type, Allocator>;

  UpdatableSnapshotTable() = default;

  explicit UpdatableSnapshotTable(base::PolymorphismEpochController* epoch)
      : SnapshotBase(epoch) {}

  ~UpdatableSnapshotTable() {
    int released_count = 0;
    if (!this->retired_values_.empty()) {
      for (auto* value: this->retired_values_) {
        released_count++;
        this->allocator_->Release(value);
      }
    }
    released_count += this->ReleaseAll_();
    LITE_LOG(INFO) << typename_ << " destruction of UpdatableSnapshotTable:" << this << " release " << released_count
                   << " object";
  }

  const ValueType* Seek(uint64_t key) {
    uint32_t bucket_index = key & (this->bucket_size_ - 1);

    auto* node = this->buckets_[bucket_index];
    auto* end = this->buckets_[bucket_index + 1];

    while (node < end) {
      if (node->key == key) {
        return reinterpret_cast<const ValueType*>(node->opaque.value);
      }
      node += 1;
    }
    return nullptr;
  }

  bool Contains(const struct_type& value) {
    auto key = HashValue(Format::Id(&value));
    return Seek(key) != nullptr;
  }

  bool Contains(uint64_t key, const struct_type& value) {
    return Seek(key) != nullptr;
  }

  versioned_value_type* Update(const struct_type& value, int32_t partition, int64_t offset) {
    auto key = HashValue(Format::Id(&value));
    return Update(key, value, partition, offset);
  }

  versioned_value_type* Update(uint64_t key, const struct_type& value, int32_t partition, int64_t offset) {
    uint32_t bucket_index = key & (this->bucket_size_ - 1);

    auto* node = this->buckets_[bucket_index];
    auto* end = this->buckets_[bucket_index + 1];
    while (node < end) {
      if (node->key == key) {
        auto* old_value = node->opaque.value;
        auto* version_value = this->allocator_->Acquire(partition, offset, value);
        version_value->value = value;
        node->opaque.value = version_value;
        Retire(old_value);
        return old_value;
      }
      node += 1;
    }

    return nullptr;
  }

  void Retire(versioned_value_type* value) {
    retired_values_.push_back(value);
    auto now_ms = butil::gettimeofday_ms();
    if ((int) retired_values_.size() > FLAGS_retire_value_batch_size
        || now_ms - last_retire_timestamp_ms_ > retire_value_interval_ms_) {
      this->epoch_->RetireUntilDone(GarbageCollector::Build(retired_values_, this->allocator_));
      last_retire_timestamp_ms_ = now_ms;
    }
  }

 protected:
  int BuildIndex_(const char* data_buffer) override {
    if (this->bucket_size_ == 0) {
      this->bucket_size_ = 1;
      this->buckets_.reset(new Node*[this->bucket_size_ + 1]());
      return 0;
    }
    this->buckets_.reset(new Node*[this->bucket_size_ + 1]);
    butil::Timer timer{butil::Timer::STARTED};
    int32_t last_bucket_index = -1;
    std::vector<versioned_value_type*> elements;
    elements.resize(this->record_count_);
    this->allocator_->MutexAcquireBatch(elements, this->record_count_, 0, 0);
    timer.stop();
    LOG(INFO) << typename_ << " Allocate " << this->record_count_ << " elements"
              << " cost " << timer.m_elapsed() << " ms"
              << " elements: " << elements.size();
    /** 统计数据哈希分布
    std::vector<int> record_hash;
    record_hash.resize(record_count_);
     */
    for (uint32_t i = 0; i < this->record_count_; ++i) {
      auto* node = &this->index_buffer_[i];
      uint64_t offset = node->opaque.offset;
      auto* versioned_value = elements[i];
      Format::StructFromFlatbuffers(
          &versioned_value->value,
          reinterpret_cast<const flatbuffers_type*>(&data_buffer[offset])
      );
      node->opaque.value = versioned_value;

      int32_t bucket_index = (int32_t) (node->key % this->bucket_size_);
      /** 统计哈希数据分布
      record_hash[i] = bucket_index;
       */
      if (bucket_index != last_bucket_index) {
        for (int32_t idx = last_bucket_index + 1; idx <= bucket_index; idx++) {
          this->buckets_[idx] = &this->index_buffer_[i];
        }
        last_bucket_index = bucket_index;
      }
    }

    for (int32_t idx = last_bucket_index + 1; idx <= (int32_t) this->bucket_size_; idx++) {
      this->buckets_[idx] = &this->index_buffer_[this->record_count_];
    }

    LITE_LOG(INFO) << typename_ << " Load complete records:" << this->record_count_
                   << " buckets:" << this->bucket_size_;
    return 0;
  }

  std::vector<versioned_value_type*> retired_values_;
  int64_t last_retire_timestamp_ms_{0};
  int retire_value_interval_ms_{60000};
};

} // namespace index_engine
