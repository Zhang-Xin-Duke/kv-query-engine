#pragma once
#include <ostream>

#include "absl/container/flat_hash_map.h"
#include "index_engine/table/neighbor_table.h"
#include "index_engine/table/purge.h"
#include "index_engine/table/updatable_snapshot_table.h"

DECLARE_double(hash_refactor);

namespace index_engine {

namespace {

inline static std::string LevelsDistribute(const std::vector<int>& level_elems, size_t total) {
  std::stringstream ss;
  ss << "[";
  for (size_t i = 0; i < level_elems.size(); i++) {
    ss << i << ":" << level_elems[i] << "(" << GetPercent(level_elems[i], total) << ")"
       << (i == level_elems.size() - 1 ? "" : ", ");
  }
  ss << "]";
  return ss.str();
}

} // namespace

template <typename T>
class NeighborPointerTable;
template <typename T>
class UpdatableNeighborPointerTable;
template <typename T>
class IndexCompatibleTable;

template <typename T>
class Seeker {
 public:
  using ValueType = typename T::ValueType;

  Seeker(T* pointer)
      : pointer_(pointer) {}
  const ValueType* Seek(uint64_t key) {
    uint32_t bucket_index = key & (pointer_->bucket_size_ - 1);
    uint64_t bucket_key = pointer_->neighbor_buckets_[bucket_index].key;

    while (bucket_key != key) {
      auto neighbor_offset = pointer_->neighbor_buckets_[bucket_index].NeighborOffset();
      if (neighbor_offset == 0) {
        return nullptr;
      }
      bucket_index += neighbor_offset;
      bucket_key = pointer_->neighbor_buckets_[bucket_index].key;
    }
    return reinterpret_cast<const ValueType*>(pointer_->neighbor_buckets_[bucket_index].PointerAddr());
  }

 private:
  T* pointer_;
};

template <typename V>
class NeighborTableBuilder {
 private:
  DEFINE_TYPE_NAME(NeighborTableBuilder<V>);

 public:
  NeighborTableBuilder() = default;
  explicit NeighborTableBuilder(uint64_t bucket_size)
      : bucket_size_(bucket_size) {}
  ~NeighborTableBuilder() {
    for (uint64_t i = 0; i < bucket_size_; i++) {
      if (link_buckets_ && link_buckets_[i]) {
        link_buckets_[i]->ReleaseLinkAsHead();
        delete link_buckets_[i];
      }
    }
  }

  bool Init() {
    if (bucket_size_ != 0 && !IsPowerOfTwo(bucket_size_)) {
      LITE_LOG(WARNING) << typename_ << " bucket size should be power of two, bucket_size: " << bucket_size_;
      return false;
    }
    link_buckets_.reset(new NeighborLinkNode*[bucket_size_]());
    return true;
  }

  void Add(uint64_t key, V* pointer) {
    CheckExpand_();
    elem_count_++;
    auto bucket_index = BucketIndex(key);
    if (link_buckets_[bucket_index]) {
      link_buckets_[bucket_index]->AddToTail(key, pointer);
    } else {
      link_buckets_[bucket_index] = new NeighborLinkNode(key, pointer);
    }
  }

  V* Upsert(uint64_t key, V* pointer) {
    CheckExpand_();
    auto bucket_index = BucketIndex(key);
    if (link_buckets_[bucket_index]) {
      auto* last_pointer = link_buckets_[bucket_index]->UpsertToTail(key, pointer);
      if (!last_pointer) {
        elem_count_++;
      }
      return last_pointer;
    } else {
      elem_count_++;
      link_buckets_[bucket_index] = new NeighborLinkNode(key, pointer);
    }
    return nullptr;
  }

  bool BuildNeighborMap() {
    if (bucket_size_ == 0) {
      bucket_size_ = 1;
      link_buckets_.reset(new NeighborLinkNode*[bucket_size_]());
      neighbor_buckets_.reset(new NeighborNode[bucket_size_]());
      return true;
    }
    neighbor_buckets_.reset(new NeighborNode[bucket_size_]());
    neighbor_jump_stat_.resize(1);

    int added_elem = BuildFromLinkNode_();
    while (added_elem > 0) {
      link_elements_stat_.emplace_back(added_elem);
      added_elem = BuildFromLinkNode_();
    }
    if (added_elem < 0) {
      LITE_LOG(WARNING) << typename_ << " fail to build neighbor table";
      return false;
    }
    LITE_LOG(INFO) << typename_ << " suss to build neighbor table"
                   << ", max_neighbor_offset: " << max_neighbor_offset_ << ", max_neighbor_jump: " << max_neighbor_jump_
                   << ", neighbor jump distribute: " << LevelsDistribute(neighbor_jump_stat_, elem_count_)
                   << ", neighbor_jump_stat_size: " << neighbor_jump_stat_.size();
    /**
    std::cout << "holder-distribute: total: " << elem_count_<< " " << butil::string_printf("%-7d", max_neighbor_offset_);
    uint64_t sum = 0;
    for (size_t i = 0; i < neighbor_jump_stat_.size(); i++) {
      sum+= neighbor_jump_stat_[i];
      std::cout << GetPercent(neighbor_jump_stat_[i], elem_count_) << "(" << GetPercent(sum, elem_count_) << ")"
                << butil::string_printf("%-7d", neighbor_jump_stat_[i]) << " ";
    }
    std::cout << std::endl;
     */

    return true;
  }

  int BucketIndex(uint64_t key) {
    return key & (bucket_size_ - 1);
  }

  uint64_t bucket_size() {
    return this->bucket_size_;
  }

  void set_bucket_size(int bucket_size) {
    this->bucket_size_ = (uint64_t) bucket_size;
  }

  class NeighborLinkNode : private ObjectCounter<NeighborLinkNode> {
   public:
    NeighborLinkNode(uint64_t key, V* pointer)
        : key_(key),
          pointer_(pointer) {}
    uint64_t key() const {
      return key_;
    }
    V* pointer() const {
      return pointer_;
    }
    NeighborLinkNode* next() const {
      return next_;
    }

    // inject compare function
    void AddToTail(uint64_t key, V* pointer) {
      if (next_) {
        next_->AddToTail(key, pointer);
      } else {
        next_ = new NeighborLinkNode(key, pointer);
      }
    }

    void AddToTail(NeighborLinkNode* node) {
      if (next_) {
        next_->AddToTail(node);
      } else {
        next_ = node;
      }
    }

    V* UpsertToTail(uint64_t key, V* pointer) {
      auto* last_pointer = pointer_;
      if (key_ == key) {
        pointer_ = pointer;
        return last_pointer;
      }
      if (next_) {
        return next_->UpsertToTail(key, pointer);
      }
      next_ = new NeighborLinkNode(key, pointer);
      return nullptr;
    }

    void ReleaseLinkAsHead() {
      if (next_ != nullptr) {
        next_->ReleaseLinkAsHead();
        delete next_;
      }
    }

    void ClearNext() {
      next_ = nullptr;
    }

   private:
    uint64_t key_;
    V* pointer_;
    NeighborLinkNode* next_{nullptr};
  };

  NeighborLinkNode* link_node(size_t index) {
    return link_buckets_[index];
  }

 private:
  template <typename T>
  friend class NeighborPointerTable;
  template <typename T>
  friend class UpdatableNeighborPointerTable;
  template <typename T>
  friend class IndexCompatibleTable;

  void CheckExpand_() {
    if (bucket_size_ * FLAGS_hash_refactor < elem_count_) {
      LITE_LOG(INFO) << typename_ << " bucket_size not enough, need expand, element_count: " << elem_count_
                     << " bucket_size: " << bucket_size_ << " refactor: " << FLAGS_hash_refactor
                     << " refactor bucket_size to : " << bucket_size_ * 2;
      Expand_();
    }
  }

  void Expand_() {
    auto last_bucket_size = bucket_size_;
    bucket_size_ *= 2;
    std::unique_ptr<NeighborLinkNode*[]> expand_link_buckets(new NeighborLinkNode*[bucket_size_]());
    for (uint64_t i = 0; i < last_bucket_size; i++) {
      auto* node = link_buckets_[i];
      while (node) {
        auto* next = node->next();
        AppendLinkNode_(expand_link_buckets, node);
        node = next;
      }
    }
    link_buckets_ = std::move(expand_link_buckets);
    LITE_LOG(INFO) << typename_ << " expand bucket_size from " << last_bucket_size << " expand to " << bucket_size_;
  }

  void AppendLinkNode_(std::unique_ptr<NeighborLinkNode*[]>& expand_link_buckets, NeighborLinkNode* link_node) {
    link_node->ClearNext();
    auto bucket_index = BucketIndex(link_node->key());
    if (expand_link_buckets[bucket_index]) {
      expand_link_buckets[bucket_index]->AddToTail(link_node);
    } else {
      expand_link_buckets[bucket_index] = link_node;
    }
  }

  int BuildFromLinkNode_() {
    int added_elem = 0;
    for (size_t i = 0; i < bucket_size_; i++) {
      auto* link_node = link_buckets_[i];
      if (!link_node) {
        continue;
      }
      auto* next = link_node->next();
      auto unset_next_node_index = i;
      auto* neighbor_node = &neighbor_buckets_[i];
      short neighbor_offset = 0;
      int neighbor_jump = 0;
      if (neighbor_node->HasValue()) {
        neighbor_jump++;
        while (neighbor_node->NeighborOffset() != 0) {
          neighbor_jump++;
          unset_next_node_index += neighbor_node->NeighborOffset();
          neighbor_node = &neighbor_buckets_[unset_next_node_index];
        }

        if (!FindNextEmptyOffset_(unset_next_node_index, neighbor_offset)) {
          LITE_LOG(WARNING) << typename_ << " can not find valid neighbor_offset";
          return -1;
        }

        neighbor_node->SetOffset(neighbor_offset);
      }
      if (neighbor_jump > max_neighbor_jump_) {
        max_neighbor_jump_ = neighbor_jump;
        neighbor_jump_stat_.resize(max_neighbor_jump_ + 1);
      }
      neighbor_jump_stat_[neighbor_jump]++;

      neighbor_node = &neighbor_buckets_[unset_next_node_index + neighbor_offset];
      neighbor_node->key = link_node->key();
      neighbor_node->SetPointer(link_node->pointer());
      /**
      LOG(INFO) << typename_ << " build neighbor node, bucket_index: " << i
                << " neighbor_offset: " << neighbor_offset
                << " unset_next_node_index: " << (unset_next_node_index+neighbor_offset)
                << " node: " << *neighbor_node
                << " value: " << *link_node->pointer();
      */
      added_elem++;
      delete (link_node);
      link_buckets_[i] = next;
    }
    return added_elem;
  }

  bool FindNextEmptyOffset_(int bucket_index, short& neighbor_offset) {
    NextElement next_element(bucket_index, bucket_size_);
    int next = next_element.Next();
    if (next == -1) {
      LITE_LOG(WARNING) << typename_ << " can not find next element, "
                        << " bucket_index: " << bucket_index << " bucket_size: " << bucket_size_;
      return false;
    }
    while (neighbor_buckets_[next].HasValue()) {
      next = next_element.Next();
      if (next == -1) {
        LITE_LOG(WARNING) << typename_ << " can not find next element";
        return false;
      }
    }
    if (next - bucket_index < -32768 || next - bucket_index > 32767) return false;
    neighbor_offset = (short) (next - bucket_index);

    if (abs(next - bucket_index) > max_neighbor_offset_) {
      max_neighbor_offset_ = abs(next - bucket_index);
      // LITE_LOG(INFO) << "new max_neighbor_offset: " << max_neighbor_offset_;
    }
    if (abs(next - bucket_index) > kMaxNeighborOffset) {
      LITE_LOG(WARNING) << typename_ << " invalid neighbor offset, more than 16 bits can save, neighbor offset: "
                        << abs(next - bucket_index);
      return false;
    }
    // LITE_LOG(INFO) << typename_ << " find neighbor_offset, bucket_index: " << bucket_index << " neighbor_offset: " << neighbor_offset << " next: " << next;
    return true;
  }

  uint64_t bucket_size_{0};
  std::unique_ptr<NeighborLinkNode*[]> link_buckets_;
  std::unique_ptr<NeighborNode[]> neighbor_buckets_;
  size_t elem_count_{0};
  int max_neighbor_offset_{0};
  int max_neighbor_jump_{0};
  std::vector<int> link_elements_stat_;
  std::vector<int> neighbor_jump_stat_;
};

template <typename Format>
class NeighborPointerTable : public SnapshotTableBase<
                                 typename Format::struct_type,
                                 NeighborPointerTable<Format>,
                                 DefaultAllocator<typename Format::struct_type>> {
 private:
  DEFINE_TYPE_NAME(NeighborPointerTable<Format>);

 public:
  using struct_type = typename Format::struct_type;
  using flatbuffers_type = typename Format::flatbuffers_type;
  using ValueType = typename Format::struct_type;
  using SnapshotBase =
      SnapshotTableBase<struct_type, NeighborPointerTable<Format>, DefaultAllocator<typename Format::struct_type>>;
  using Node = typename SnapshotBase::Node;

  NeighborPointerTable() = default;
  explicit NeighborPointerTable(base::PolymorphismEpochController* epoch)
      : SnapshotBase(epoch) {}
  ~NeighborPointerTable() {
    int released_count = this->ReleaseAll_();
    LITE_LOG(INFO) << " destruction of " << this->typename_ << " addr: " << this << " release " << released_count
                   << " object";
  }

  template <typename>
  friend class Seeker;
  const ValueType* Seek(uint64_t key) {
    //return seeker_.Seek(key);
    uint32_t bucket_index = key & (this->bucket_size_ - 1);
    uint64_t bucket_key = neighbor_buckets_[bucket_index].key;

    while (bucket_key != key) {
      auto neighbor_offset = neighbor_buckets_[bucket_index].NeighborOffset();
      if (neighbor_offset == 0) {
        return nullptr;
      }
      bucket_index += neighbor_offset;
      bucket_key = neighbor_buckets_[bucket_index].key;
    }
    return reinterpret_cast<const ValueType*>(neighbor_buckets_[bucket_index].PointerAddr());
  }

  void Visit(const std::function<void(uint64_t, struct_type*)>& visitor) override {
    for (uint64_t i = 0; i < this->bucket_size_; i++) {
      auto& node = neighbor_buckets_[i];
      // LITE_LOG(INFO) << "node info:" << node;
      if (node.HasValue()) {
        visitor(node.key, reinterpret_cast<struct_type*>(node.PointerAddr()));
      }
    }
  }

  std::string Status() const {
    std::stringstream ss;
    ss << "{element_count: " << this->record_count_ << ", bucket_size: " << this->bucket_size_
       << ", max_neighbor_offset: " << this->max_neighbor_offset_
       << ", neighbor jump distribute: " << LevelsDistribute(this->neighbor_jump_stat_, this->record_count_) << "}";
    return ss.str();
  }

  friend std::ostream& operator<<(std::ostream& os, const NeighborPointerTable& table) {
    os << table.Status();
    return os;
  }

 protected:
  int BuildIndex_(const char* flatbuffers_buffer) override {
    butil::Timer timer{butil::Timer::STARTED};
    if (this->bucket_size_ * FLAGS_hash_refactor < this->record_count_) {
      LITE_LOG(INFO) << typename_ << " hash refactor, record_count: " << this->record_count_
                     << " bucket_size: " << this->bucket_size_
                     << " refactor bucket_size to : " << this->bucket_size_ * 2;
      this->bucket_size_ *= 2;
    }

    NeighborTableBuilder<struct_type> neighbor_table_builder(this->bucket_size_);
    // LITE_LOG(INFO) << typename_ << " start to build neighbor_table_builder";
    if (!neighbor_table_builder.Init()) {
      LITE_LOG(WARNING) << typename_ << " fail to init NeighborTableBuilder";
      return -1;
    }
    // LITE_LOG(INFO) << typename_ << " neighbor_table_builder init done, record_count: " << this->record_count_;

    for (uint32_t i = 0; i < this->record_count_; ++i) {
      auto* node = &this->index_buffer_[i];
      uint64_t offset = node->opaque.offset;
      auto* elem = this->allocator_->Acquire();
      Format::StructFromFlatbuffers(elem, reinterpret_cast<const flatbuffers_type*>(&flatbuffers_buffer[offset]));
      neighbor_table_builder.Add(node->key, elem);
    }
    this->index_buffer_.reset(nullptr);

    if (!neighbor_table_builder.BuildNeighborMap()) {
      LITE_LOG(WARNING) << typename_ << " fail to build neighbor table";
      return -1;
    }
    neighbor_buckets_.swap(neighbor_table_builder.neighbor_buckets_);
    timer.stop();
    LITE_LOG(INFO) << typename_ << " Load complete records:" << this->record_count_ << " buckets:" << this->bucket_size_
                   << " cost: " << timer.m_elapsed() << " ms";
    this->max_neighbor_offset_ = neighbor_table_builder.max_neighbor_offset_;
    this->max_neighbor_jump_ = neighbor_table_builder.max_neighbor_jump_;
    this->neighbor_jump_stat_.swap(neighbor_table_builder.neighbor_jump_stat_);
    this->bucket_size_ = neighbor_table_builder.bucket_size_;
    return 0;
  }

 private:
  std::unique_ptr<NeighborNode[]> neighbor_buckets_;
  int max_neighbor_offset_;
  int max_neighbor_jump_;
  std::vector<int> neighbor_jump_stat_;
  Seeker<NeighborPointerTable<Format>> seeker_{this};
};

template <class Format>
class UpdatableNeighborPointerTable
    : public SnapshotTableBase<VersionedValue<typename Format::struct_type>, UpdatableNeighborPointerTable<Format>> {
 private:
  DEFINE_TYPE_NAME(UpdatableNeighborPointerTable<Format>);

 public:
  using struct_type = typename Format::struct_type;
  using flatbuffers_type = typename Format::flatbuffers_type;
  using versioned_value_type = VersionedValue<struct_type>;
  using ValueType = typename Format::ValueType;
  using SnapshotBase = SnapshotTableBase<versioned_value_type, UpdatableNeighborPointerTable<Format>>;
  using Node = typename SnapshotBase::Node;
  using Allocator = typename SnapshotBase::allocator_type;

  using GarbageCollector = BatchValueGarbageCollector<versioned_value_type, Allocator>;
  UpdatableNeighborPointerTable() = default;

  explicit UpdatableNeighborPointerTable(base::PolymorphismEpochController* epoch)
      : SnapshotBase(epoch) {}

  ~UpdatableNeighborPointerTable() {
    int released_count = 0;
    if (!this->retired_values_.empty()) {
      for (auto* value: this->retired_values_) {
        released_count++;
        this->allocator_->Release(value);
      }
    }
    if (this->inited_) {
      released_count += this->ReleaseAll_();
    }
    LITE_LOG(INFO) << "destruction of " << typename_ << this << " release " << released_count << " object";
  }

  static_assert(offsetof(VersionedValue<struct_type>, value) == 0, "struct_type must put first");
  const ValueType* Seek(uint64_t key) {
    uint32_t bucket_index = key & (this->bucket_size_ - 1);
    uint64_t bucket_key = neighbor_buckets_[bucket_index].key;

    while (bucket_key != key) {
      auto neighbor_offset = neighbor_buckets_[bucket_index].NeighborOffset();
      if (neighbor_offset == 0) {
        return nullptr;
      }
      bucket_index += neighbor_offset;
      bucket_key = neighbor_buckets_[bucket_index].key;
    }
    return reinterpret_cast<const ValueType*>(neighbor_buckets_[bucket_index].PointerAddr());
  }

  versioned_value_type* Seek(uint64_t key, uint32_t& bucket_index) {
    bucket_index = key & (this->bucket_size_ - 1);
    uint64_t bucket_key = neighbor_buckets_[bucket_index].key;

    while (bucket_key != key) {
      auto neighbor_offset = neighbor_buckets_[bucket_index].NeighborOffset();
      if (neighbor_offset == 0) {
        return nullptr;
      }
      bucket_index += neighbor_offset;
      bucket_key = neighbor_buckets_[bucket_index].key;
    }
    return reinterpret_cast<versioned_value_type*>(neighbor_buckets_[bucket_index].PointerAddr());
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
    uint64_t bucket_key = neighbor_buckets_[bucket_index].key;

    while (bucket_key != key) {
      auto neighbor_offset = neighbor_buckets_[bucket_index].NeighborOffset();
      if (neighbor_offset == 0) {
        return nullptr;
      }
      bucket_index += neighbor_offset;
      bucket_key = neighbor_buckets_[bucket_index].key;
    }

    auto* old_value = neighbor_buckets_[bucket_index].PointerAddr();
    auto* version_value = this->allocator_->Acquire(partition, offset, value);
    neighbor_buckets_[bucket_index].SetPointer(version_value);
    Retire(reinterpret_cast<versioned_value_type*>(old_value));
    return reinterpret_cast<versioned_value_type*>(old_value);
  }

  void Retire(versioned_value_type* value) {
    if (!value)
      return;
    retired_values_.push_back(value);
    auto now_ms = butil::gettimeofday_ms();
    if ((int) retired_values_.size() > FLAGS_retire_value_batch_size
        || now_ms - last_retire_timestamp_ms_ > retire_value_interval_ms_) {
      this->epoch_->RetireUntilDone(GarbageCollector::Build(retired_values_, this->allocator_));
      last_retire_timestamp_ms_ = now_ms;
    }
  }

  void Visit(const std::function<void(uint64_t, versioned_value_type*)>& visitor) override {
    for (uint64_t i = 0; i < this->bucket_size_; i++) {
      auto& node = neighbor_buckets_[i];
      // LITE_LOG(INFO) << "node info:" << node;
      if (node.HasValue()) {
        visitor(node.key, reinterpret_cast<versioned_value_type*>(node.PointerAddr()));
      }
    }
  }

  std::string Status() const {
    std::stringstream ss;
    ss << "{element_count: " << this->record_count_ << ", bucket_size: " << this->bucket_size_
       << ", max_neighbor_offset: " << this->max_neighbor_offset_
       << ", neighbor jump distribute: " << LevelsDistribute(this->neighbor_jump_stat_, this->record_count_) << "}";
    return ss.str();
  }

 protected:
  int BuildIndex_(const char* data_buffer) override {
    butil::Timer timer{butil::Timer::STARTED};
    if (this->bucket_size_ * FLAGS_hash_refactor < this->record_count_) {
      LITE_LOG(INFO) << "hash refactor, record_count: " << this->record_count_ << " bucket_size: " << this->bucket_size_
                     << " refactor bucket_size to : " << this->bucket_size_ * 2;
      this->bucket_size_ *= 2;
    }

    NeighborTableBuilder<VersionedValue<struct_type>> neighbor_table_builder(this->bucket_size_);
    // LITE_LOG(INFO) << "start to build neighbor_table_builder";
    if (!neighbor_table_builder.Init()) {
      LITE_LOG(WARNING) << typename_ << " fail to init NeighborTableBuilder";
      return -1;
    }
    // LITE_LOG(INFO) << "neighbor_table_builder init done, record_count: " << this->record_count_;
    std::vector<versioned_value_type*> elements;
    elements.resize(this->record_count_);
    this->allocator_->MutexAcquireBatch(elements, this->record_count_, 0, 0);

    for (uint32_t i = 0; i < this->record_count_; ++i) {
      auto* node = &this->index_buffer_[i];
      uint64_t offset = node->opaque.offset;
      auto* versioned_value = elements[i];

      Format::StructFromFlatbuffers(
          &versioned_value->value,
          reinterpret_cast<const flatbuffers_type*>(&data_buffer[offset])
      );
      neighbor_table_builder.Add(node->key, versioned_value);
    }
    this->index_buffer_.reset(nullptr);

    if (!neighbor_table_builder.BuildNeighborMap()) {
      LITE_LOG(WARNING) << typename_ << " fail to build neighbor table";
      return -1;
    }
    neighbor_buckets_.swap(neighbor_table_builder.neighbor_buckets_);
    timer.stop();
    LITE_LOG(INFO) << typename_ << " Load complete records:" << this->record_count_ << " buckets:" << this->bucket_size_
                   << " cost: " << timer.m_elapsed() << " ms";
    this->max_neighbor_offset_ = neighbor_table_builder.max_neighbor_offset_;
    this->max_neighbor_jump_ = neighbor_table_builder.max_neighbor_jump_;
    this->neighbor_jump_stat_.swap(neighbor_table_builder.neighbor_jump_stat_);
    this->bucket_size_ = neighbor_table_builder.bucket_size_;
    return 0;
  }

  std::unique_ptr<NeighborNode[]> neighbor_buckets_;
  int max_neighbor_offset_;
  int max_neighbor_jump_;
  std::vector<int> neighbor_jump_stat_;
  std::vector<versioned_value_type*> retired_values_;
  int64_t last_retire_timestamp_ms_{0};
  int retire_value_interval_ms_{60000};
};

} // namespace index_engine
