#pragma once

#include <math.h>

#include "absl/container/flat_hash_map.h"
#include "butil/containers/flat_map.h"
#include "butil/time.h"
#include "index_engine/table/purge.h"
#include "index_engine/util/allocator.h"
#include "purge.h"

DECLARE_double(hash_refactor);

namespace index_engine {

enum FindDirection { kBoth, kOnlyLeft, kOnlyRight, kBlocked };
enum LastDirection { kRight, kLeft };

inline FindDirection GetFindDirection(int64_t index, int64_t size) {
  if (size <= 1 || index >= size) {
    return kBlocked;
  }
  if (index == size - 1) {
    return kOnlyLeft;
  }
  if (index == 0) {
    return kOnlyRight;
  }
  return kBoth;
}

// 其实不需要这么麻烦,两边硬找就行,写都写了,就这么用吧
class NextElement {
 public:
  NextElement(int64_t index, int64_t size)
      : offset_(0),
        index_(index),
        size_(size),
        find_direction_(GetFindDirection(index, size)),
        last_direction_(find_direction_ == kOnlyLeft ? kRight : kLeft) {}

  int Next() {
    switch (find_direction_) {
      case kBlocked:
        return -1;
      case kBoth:
        return BothFind_();
      case kOnlyLeft:
        return OnlyLeftFind_();
      case kOnlyRight:
        return OnlyRightFind_();
    }
    return -1;
  }

 private:
  int BothFind_() {
    int ret;
    if (last_direction_ == kLeft) {
      offset_ = -offset_ + 1;
      ret = offset_ + index_;
      if (index_ + offset_ == size_ - 1) {
        find_direction_ = kOnlyLeft;
        offset_ = -offset_ + 1;
      }
      last_direction_ = kRight;
    } else {
      offset_ = -offset_;
      ret = index_ + offset_;
      if (index_ + offset_ == 0) {
        find_direction_ = kOnlyRight;
        offset_ = -offset_;
      }
      last_direction_ = kLeft;
    }
    return ret;
  }
  int OnlyLeftFind_() {
    offset_ = offset_ - 1;
    if (index_ + offset_ == 0) {
      find_direction_ = kBlocked;
    }
    return index_ + offset_;
  }
  int OnlyRightFind_() {
    offset_ = offset_ + 1;
    if (index_ + offset_ == size_ - 1) {
      find_direction_ = kBlocked;
    }
    return index_ + offset_;
  }
  int64_t offset_;
  int64_t index_;
  int64_t size_;
  FindDirection find_direction_;
  LastDirection last_direction_;
};

/** 2**16 */
ALLOW_UNUSED const static int kMaxNeighborOffset = 32768;
ALLOW_UNUSED const static int kOffsetLen = 16;
ALLOW_UNUSED const static int kPointerLen = 48;
ALLOW_UNUSED const static uint32_t kNeighborOffsetMask = 0xffff;
ALLOW_UNUSED const static uint64_t kNeighborPointerMask = (1l << kPointerLen) - 1;
ALLOW_UNUSED const static uint64_t kNeighborHeaderOffsetMask = ((1l << kOffsetLen) - 1) << kPointerLen;
ALLOW_UNUSED const static int kMaxBatchSize = 1024;
ALLOW_UNUSED const static int kMaxBatchTimeoutMills = 1000;

struct NeighborNode : private ObjectCounter<NeighborNode> {
  uint64_t key{0};
  uint64_t neighbor_and_pointer{0};
  short NeighborOffset() const {
    return short((neighbor_and_pointer >> kPointerLen) & kNeighborOffsetMask);
  }

  void* PointerAddr() const {
    return (void*) (neighbor_and_pointer & kNeighborPointerMask);
  }
  void SetKey(uint64_t k) {
    key = k;
  }
  void SetOffset(short offset) {
    uint64_t value = (uint64_t(static_cast<uint16_t>(offset)) << kPointerLen) | (neighbor_and_pointer & kNeighborPointerMask);
    SetNeighborAndPointer(value);
  }
  void SetPointer(void* addr) {
    if (reinterpret_cast<uintptr_t>(addr) & ~kNeighborPointerMask) throw std::range_error("pointer exceeds 48 bits");
    uint64_t value = ((neighbor_and_pointer & kNeighborHeaderOffsetMask) | (uint64_t) addr);
    SetNeighborAndPointer(value);
  }
  void SetOffsetPointer(short offset, void* pointer) {
    if (reinterpret_cast<uintptr_t>(pointer) & ~kNeighborPointerMask) throw std::range_error("pointer exceeds 48 bits");
    uint64_t value = (uint64_t(static_cast<uint16_t>(offset)) << kPointerLen) | ((uint64_t) pointer & kNeighborPointerMask);
    SetNeighborAndPointer(value);
  }
  void SetNeighborAndPointer(uint64_t value) {
    neighbor_and_pointer = value;
  }
  bool HasValue() const {
    return neighbor_and_pointer != 0;
  }
  friend std::ostream& operator<<(std::ostream& os, const NeighborNode& node) {
    os << "(" << node.key << "," << node.NeighborOffset() << "," << node.PointerAddr() << ")";
    return os;
  }
};

#if __cplusplus >= 202002L
template <typename V, PurgeConcept Purge, typename>
#else
template <typename, typename, typename>
#endif
class NeighborTable;

#if __cplusplus >= 202002L
template <typename V, PurgeConcept Purge>
#else
template <typename V, typename Purge>
#endif
class NeighborBlock {
  using NeighborBlockType = NeighborBlock<V, Purge>;

 private:
  DEFINE_TYPE_NAME(NeighborBlockType);

 public:
  using BucketsType = std::unique_ptr<NeighborNode[]>;

  NeighborBlock() {
    LOG(INFO) << "construction of NeighborBlock and not initialize, block: " << this;
  }

  NeighborBlock(uint64_t bucket_size, Purge* purge)
      : purge_(purge),
        bucket_size_(butil::find_power2(bucket_size)) {
    if (bucket_size_ == 0)
      bucket_size_ = 1;
    LITE_LOG(INFO) << "construction of NeighborBlock with bucket_size: " << bucket_size_ << " block: " << this;
    buckets_ = std::make_unique<NeighborNode[]>(bucket_size_);
  }

  ~NeighborBlock() {
    LITE_LOG(INFO) << "destruction of NeighborBlock block: " << this;
  }

  void ReadBlock(std::string& data) {
    auto memory_size = sizeof(NeighborNode) * bucket_size_;
    data.resize(memory_size);
    LITE_LOG(INFO) << "read block, memory size: " << memory_size;
    data.resize(sizeof(NeighborNode) * bucket_size_);
    memcpy((void*) data.data(), buckets_.get(), sizeof(NeighborNode) * bucket_size_);
  }

  void ExpandFrom(NeighborBlock* other) {
    other->FlushKickoff();
    for (uint64_t i = 0; i < other->bucket_size_; i++) {
      if (other->buckets_[i].HasValue()) {
        KickUpsert(other->buckets_[i].key, (V*) other->buckets_[i].PointerAddr());
      }
    }
    LITE_LOG(INFO) << "NeighborBlock expand, current size: " << size_ << " bucket_size: " << bucket_size_
                   << " last size: " << other->size_ << " last bucket_size: " << other->bucket_size_;
  }

  const V* Seek(uint64_t key) const {
    uint32_t bucket_index = BucketIndex(key);
    uint64_t bucket_key = buckets_[bucket_index].key;

    while (bucket_key != key) {
      auto neighbor_offset = buckets_[bucket_index].NeighborOffset();
      if (neighbor_offset == 0) {
        return nullptr;
      }
      bucket_index += neighbor_offset;
      bucket_key = buckets_[bucket_index].key;
    }
    return reinterpret_cast<const V*>(buckets_[bucket_index].PointerAddr());
  }

  void Traverse(std::vector<uint64_t>& keys, std::vector<const V*>& values) const {
    for (uint64_t i = 0; i < bucket_size_; i++) {
      if (buckets_[i].HasValue()) {
        keys.emplace_back(buckets_[i].key);
        values.emplace_back(reinterpret_cast<const V*>(buckets_[i].PointerAddr()));
      }
    }
  }

  enum UpsertStatus { kInsert, kUpdate, kKickOff };
  using UpsertResult = std::pair<UpsertStatus, V*>;

  void KickUpsert(uint64_t key, V* pointer) {
    if (UpsertImpl_(key, pointer, false).first == kInsert) {
      size_++;
    }
  }

  // autoflush: 是否检查需要自动同步下kickoff数据,初始化时其实无所谓,检查了也很快.
  //            现在的主要用途是批处理时,比如一次加100w条数据,不开autoflush,可以等所有的100w都找好位置后一次更新
  //
  // return:
  //  nullptr  insert new elment on empty bucket
  //  lastptr  update the old ptr to pointer, old_ptr need recycle
  //  pointer  rechain the bucket and move out position for pointer, reassign in batch latter
  //
  // 插入结果可以分为三种:
  //  1. 新增(初始位置或者尾部新增)
  //  2. 更新
  //  3. 踢出,对应bucket已被其他元素占用,要给占此位置的元素找个新的位置,重新链上,再将新的元素放到此位置
  //
  // 对于非0位置的新增和更新操作,对于读完全没有影响,读操作总能读到预期内的数据
  // 由于bucket的默认值是0,假设bucket0上的元素永远是正确key的,对于位置0,在新增元素时,需要先给key赋值,在给value赋值
  // 若给value赋值,key 0的查询有可能读到错误的数据
  //
  // kick_off数据在处理时需要同步两次
  // 第一次同步用于保证再也没有next指向被kick的元素,保证kick修改过程中,reader不会读到错误的数据
  // 非0位置的赋值,需要先给value赋值,再给key赋值,这样可以保证不会读到错误的值
  // 第二次同步就用于保证赋value肯定发生在赋key之前,reader看到新的相同bucket_index key时,对应的pointer一定已经被更新过
  V* Upsert(uint64_t key, V* pointer, bool auto_flush = true) {
    if (auto_flush) {
      CheckDelayBatchInsert_();
    }

    // LOG(INFO) << typename_ << " upsert key: " << key << " pointer: " << pointer;
    auto upsert_result = UpsertImpl_(key, pointer, true);
    // 插入了新的元素,且已经插入到表中,可以被同步查询到
    if (upsert_result.first == kInsert) {
      // LOG(INFO) << "key: " << key << " last pointer nullptr, pointer: " << pointer;
      size_++;
      return nullptr;
    }
    // 更新原有元素
    if (upsert_result.first == kUpdate) {
      // LOG(INFO) << "key: " << key << " update pointer, last: " << upsert_result.second << " pointer: " << pointer;
      purge_->NodeExpire(upsert_result.second);
      return upsert_result.second;
    }

    // 这里有两种情况
    // 一种是有要踢开的元素,且踢开了原来的元素
    // 一种是在这一批操作中,前边已经有相同位置的踢开了元素
    // 两个完全相同元素的key需要用新的替换老的,同时释放老的
    // 两个求余后相同的元素,需要在批量批量插入时,按照顺序串起来
    auto it = kickoff_elems_.find(key);
    V* last = nullptr;

    if (it == kickoff_elems_.end()) {
      // LITE_LOG(INFO) << "kickoff_elems not kicked, key: " << key << "  pointer: " << pointer;
      // 添加新的元素
      size_++;
    } else {
      // LITE_LOG(INFO) << "kickoff_elems already kicked, key: " << key << "  pointer: " << pointer;
      // 同一个key多次更新,使用新的key,回收老的key
      last = it->second;
      purge_->NodeExpire(last);
    }

    // 新的要更新的元素,加到kickoff_elems_中,准备批量插入,其中可能有bucket_index相同的元素
    kickoff_elems_[key] = pointer;
    return last;
  }

  uint64_t BucketIndex(uint64_t key) const {
    return key & (bucket_size_ - 1);
  }

  uint64_t Size() const {
    return size_;
  }

  uint64_t BucketSize() const {
    return bucket_size_;
  }

  bool Full() const {
    return (uint64_t) (bucket_size_ * FLAGS_hash_refactor) <= size_;
  }

  void FlushKickoff() {
    if (kickoff_elems_.empty()) {
      // LOG(INFO) << "kickoff_elems empty, skip DelayBatchInsert";
      return;
    }

    purge_->epoch()->BumpVersionUntilDone();
    purge_->WaitReadRelease(waitfor_pending_version_);
    // 第一轮,等待没有能通过next过来的read
    // [{pos, key}]
    std::vector<std::pair<uint64_t, uint64_t>> pos_key_pairs;
    std::set<uint64_t> token_poses;
    // 找位置,并更新所有value
    auto it = kickoff_elems_.begin();
    while (it != kickoff_elems_.end()) {
      auto key = it->first;
      auto bucket_index = BucketIndex(key);
      if (token_poses.count(bucket_index) == 0) {
        // 原始值是被kick_off的,bucket_index对不上
        buckets_[bucket_index].SetOffsetPointer(0, it->second);
        pos_key_pairs.emplace_back(bucket_index, key);
        token_poses.insert(bucket_index);
        /**
        LITE_LOG(INFO) << typename_ << " kick off element,"
                       << " bucket_index: " << bucket_index
                       << " key: " << key
                       << " pointer: " << it->second;
        */
      } else {
        // 同一批中有多个会映射到相同bucket_index的元素,找个新位置给放末尾
        auto neighbor_offset = buckets_[bucket_index].NeighborOffset();
        while (neighbor_offset != 0) {
          bucket_index += buckets_[bucket_index].NeighborOffset();
          neighbor_offset = buckets_[bucket_index].NeighborOffset();
        }
        short nearest_offset = 0;
        if (!FindNearestEmptyOffset_(bucket_index, nearest_offset)) {
          // 这里其实有些问题,当真的抛出异常的时候,会有一些for循环之前的处理了一半的异常数据残留
          // 基本上只存在理论发生的可能性(满的情况之前的Upsert就会抛异常)
          // 处理的话影响吞吐,abort的话又过于暴力,这里抛出异常外围打个FATAL
          LITE_LOG(WARNING) << typename_ << " can not find valid neighbor_offset";
          throw std::range_error("can not find invalid neighbor");
        }
        buckets_[bucket_index + nearest_offset].SetPointer(it->second);
        buckets_[bucket_index].SetOffset(nearest_offset);
        pos_key_pairs.emplace_back(bucket_index + nearest_offset, key);
        /**
        LITE_LOG(INFO) << typename_ << " add to kick off element tail,"
                       << " bucket_index: " << (bucket_index + nearest_offset)
                       << " key: " << key
                       << " pointer: " << it->second;
         */
      }
      it++;
    }
    purge_->WaitAllReadReleased();
    // 更新所有的key,确保更新key之前value对所有读可见
    for (auto& pos_key: pos_key_pairs) {
      buckets_[pos_key.first].SetKey(pos_key.second);
    }
    // LOG(INFO) << "finish kickoff elems insert, size: " << kickoff_elems_.size();
    kickoff_elems_.clear();
  }

  template <typename Visitor>
  void ForEach(const Visitor& visitor) {
    for (uint64_t i = 0; i < bucket_size_; i++) {
      auto bucket_key = buckets_[i].key;
      if (BucketIndex(bucket_key) != i) {
        continue;
      }
      auto bucket_index = i;
      short neighbor_offset;
      do {
        neighbor_offset = buckets_[bucket_index].NeighborOffset();
        auto pointer = buckets_[bucket_index].PointerAddr();
        if (pointer) {
          visitor(bucket_key, (const V*) pointer);
        }
        bucket_index += neighbor_offset;
        bucket_key = buckets_[bucket_index].key;
      } while (neighbor_offset != 0);
    }
  }

  // 当自己根据内容直接构造neighbor数据时,需要再给指定一遍size
  void ResetSizeBucket0(uint64_t size) {
    size_ = size;
    ResetBucket0_();
    LITE_LOG(INFO) << typename_ << " reset size: " << size << " bucket0 inited: " << bucket0_inited_;
  }

  void ResetSizeBucket0() {
    for (uint64_t i = 0; i < bucket_size_; i++) {
      if (buckets_[i].HasValue()) {
        size_++;
      }
    }
    ResetBucket0_();
    LITE_LOG(INFO) << typename_ << " reset size: " << size_ << " bucket0 inited: " << bucket0_inited_;
  }

 private:
  void ResetBucket0_() {
    if (buckets_[0].HasValue()) {
      bucket0_inited_ = true;
    }
  }

  void CheckDelayBatchInsert_() {
    if (kickoff_elems_.empty()) {
      return;
    }
    auto time = butil::gettimeofday_ms();
    if (kickoff_elems_.size() >= kMaxBatchSize || time - last_batch_time_ > kMaxBatchTimeoutMills) {
      /**
      LOG_BY_INTERVAL(INFO, 1) << "NeighborBlock need delay batch insert,"
                               << " kickoff_elems size: " << kickoff_elems_.size()
                               << " last_batch_time: " << last_batch_time_
                               << " time: " << time;
      */
      FlushKickoff();
      last_batch_time_ = time;
    }
  }

  // delay_insert: 是否延迟插入,初始化时,因为没有读线程,可以直接插入;有读操作需要延迟批量插入
  // return:
  //  nullptr  在原始空的桶上插入了新的元素
  //  lastptr  将老的元素更新为新的元素,老的元素需要回收
  //  pointer  桶位置冲突,将对应元素放到空桶中,并链接到整个串上,批量插入中,一种是初次挪位置,一种是已经被前辈挪过了,已经不再对应串上
  UpsertResult UpsertImpl_(uint64_t key, V* pointer, bool delay_insert) {
    auto bucket_index = BucketIndex(key);
    auto& node = buckets_[bucket_index];

    if (!bucket0_inited_ && bucket_index == 0) {
      if (!delay_insert) {
        node.SetKey(key);
        node.SetPointer(pointer);
      } else {
        InitBucket0_(key, pointer);
      }
      bucket0_inited_ = true;
      return {kInsert, nullptr};
    }

    // case 1: 原有元素不存在,读安全,直接插入,先插key或者先插pointer没有影响
    // return: nullptr
    if (!node.HasValue()) {
      node.SetPointer(pointer);
      node.SetKey(key);
      // LOG(INFO) << "!node.HasValue set value, key: " << key << " pointer:" << pointer;
      return {kInsert, nullptr};
    }
    // case 2: 原有位置上的元素是bucket_index相同的元素,在链上查找对应的key
    //         找到: 直接更新对应值,读安全的
    //         找不到: 查找一个最近的空桶,将对应的key,pointer写入,同case 1相同,读安全
    // return: last_ptr 之前存在元素,返回前值; nullptr 之前为空,插入了空洞中
    if (bucket_index == BucketIndex(node.key)) {
      auto bucket_key = node.key;
      short nearest_offset = 0;

      while (bucket_key != key) {
        auto neighbor_offset = buckets_[bucket_index].NeighborOffset();
        // reach tail and can not find element, insert elem to tail
        if (neighbor_offset == 0) {
          if (!FindNearestEmptyOffset_(bucket_index, nearest_offset)) {
            LITE_LOG(WARNING) << typename_ << " can not find valid neighbor_offset";
            throw std::range_error("can not find invalid neighbor");
          }

          NeighborNode& new_node = buckets_[bucket_index + nearest_offset];
          new_node.SetPointer(pointer);
          new_node.SetKey(key);
          buckets_[bucket_index].SetOffset(nearest_offset);
          return {kInsert, nullptr};
        }
        bucket_index += neighbor_offset;
        bucket_key = buckets_[bucket_index].key;
      }
      // find element, update pointer
      auto* last_pointer = buckets_[bucket_index].PointerAddr();
      buckets_[bucket_index].SetPointer(pointer);
      return {kUpdate, reinterpret_cast<V*>(last_pointer)};
    }

    // case 3: 桶已经被占了,并且占用的元素不是第一个元素,需要给原有元素找一个新位置,再让原有元素上一个指针指向新位置
    //         同步时需要等待epoch,可能较慢,因此这种类型的元素统一批量更新
    //         批量更新的过程中,可能出现插入不bucket_index的元素,插入相同key的元素(挪过一次之后,下次不再串上,pre遍历找不到对应元素)
    // return: pointer 与插入相同的pointer,表示可能重排了链,但是没有真正插入元素
    auto bucket_key = node.key;

    // 第一次同步时,只需没有这个版本的读即可
    waitfor_pending_version_ = purge_->epoch()->current_version();
    // 查找前一个元素
    auto pre_bucket_index = BucketIndex(bucket_key);
    while (pre_bucket_index + buckets_[pre_bucket_index].NeighborOffset() != bucket_index) {
      // 之前已经重排过,添加过相同bucket_index的元素
      if (buckets_[pre_bucket_index].NeighborOffset() == 0) {
        return {kKickOff, nullptr};
      }
      pre_bucket_index += buckets_[pre_bucket_index].NeighborOffset();
    }
    // 查找空位
    short nearest_offset = 0;
    if (!FindNearestEmptyOffset_(pre_bucket_index, nearest_offset)) {
      LITE_LOG(WARNING) << typename_ << " can not find valid neighbor_offset";
      throw std::range_error("can not find valid neighbor_offset");
    }
    uint64_t nearest_empty_index = pre_bucket_index + nearest_offset;

    // 将空位置的下一个元素指向之前bucket的下一个元素
    if (buckets_[bucket_index].NeighborOffset() != 0) {
      int64_t kicked_and_next_offset = bucket_index + buckets_[bucket_index].NeighborOffset() - (nearest_empty_index);
      if (abs(kicked_and_next_offset) > kMaxNeighborOffset) {
        throw std::range_error("too large neighbor offset after kick");
      }
      buckets_[nearest_empty_index].SetOffset((short) kicked_and_next_offset);
    }
    // 空位置key, value设置
    buckets_[nearest_empty_index].SetKey(buckets_[bucket_index].key);
    buckets_[nearest_empty_index].SetPointer(buckets_[bucket_index].PointerAddr());

    // 将前一个元素的位置指向新插入的空位置
    buckets_[pre_bucket_index].SetOffset(nearest_offset);
    if (!delay_insert) {
      // 插入新的元素
      buckets_[bucket_index].SetKey(key);
      buckets_[bucket_index].SetOffsetPointer(0, pointer);
      return {kInsert, nullptr};
    }
    return {kKickOff, nullptr};
  }

  // 永远不返回bucket 0,保证bucket 0的元素不会被kick out,只有插入和更新两种操作
  bool FindNearestEmptyOffset_(int64_t bucket_index, short& neighbor_offset) {
    NextElement next_element(bucket_index, bucket_size_);
    int next = next_element.Next();
    if (next == -1) {
      LITE_LOG(WARNING) << typename_ << " can not find next element, "
                        << " bucket_index: " << bucket_index << " bucket_size: " << bucket_size_;
      return false;
    }
    while (buckets_[next].HasValue() || next == 0) {
      next = next_element.Next();
      if (next == -1) {
        LITE_LOG(WARNING) << typename_ << " can not find next element, bucket_index: " << bucket_index;
        return false;
      }
    }
    neighbor_offset = (short) (next - bucket_index);

    if (abs(next - bucket_index) > max_neighbor_offset_) {
      max_neighbor_offset_ = abs(next - bucket_index);
      // LITE_LOG(INFO) << "new max_neighbor_offset: " << max_neighbor_offset_;
    }
    if (abs(next - bucket_index) > kMaxNeighborOffset /** 2**16 */) {
      LITE_LOG(WARNING) << typename_ << " invalid neighbor offset, more than 16 bits can save, neighbor offset: "
                        << abs(next - bucket_index);
      return false;
    }
    // LITE_LOG(INFO) << typename_ << " find neighbor_offset, bucket_index: " << bucket_index << " neighbor_offset: " << neighbor_offset << " next: " << next;
    return true;
  }

  void InitBucket0_(uint64_t key, V* pointer) {
    if (BucketIndex(key) != 0 || buckets_[0].HasValue()) {
      CHECK(false) << "invalid bucket0 init";
      return;
    }
    buckets_[0].SetKey(key);
    purge_->WaitAllReadReleased();
    buckets_[0].SetPointer(pointer);
  }

  Purge* purge_{nullptr};
  template <typename, typename, typename>
  friend class NeighborTable;
  uint64_t size_{0};
  uint64_t bucket_size_{0};
  BucketsType buckets_;
  // Stat
  int max_neighbor_offset_{0};
  uint64_t waitfor_pending_version_{0};
  // 用于批量插入操作,单线程操作
  absl::flat_hash_map<uint64_t, V*> kickoff_elems_;
  int64_t last_batch_time_{butil::gettimeofday_ms()};
  bool bucket0_inited_{false};
};

#if __cplusplus >= 202002L
template <typename V, PurgeConcept Purge = AllocatorPurge<V>, typename Hash = AbslHash>
#else
template <typename V, typename Purge = AllocatorPurge<V>, typename Hash = AbslHash>
#endif
class NeighborTable {
  using BlockCollectType = PointerCollector<NeighborBlock<V, Purge>>;

 private:
  DEFINE_TYPE_NAME(NeighborTable);
  DISALLOW_COPY_AND_ASSIGN(NeighborTable);

 public:
  class NeighborTableReader {
   public:
    explicit NeighborTableReader(NeighborTable* table)
        : handle_(table->purge_->Acquire()),
          table_(table) {}
    ~NeighborTableReader() {
      table_->purge_->Release(handle_);
    }

    const V* Seek(uint64_t id) const {
      return table_->Seek(id);
    }

   private:
    DISALLOW_ASSIGN(NeighborTableReader);
    friend class NeighborTable<V, Purge, Hash>;
    uint64_t handle_;
    NeighborTable* table_;
  };

  NeighborTableReader AcquireReader() {
    NeighborTableReader reader(this);
    return reader;
  }

  NeighborTable()
      : own_purge_(false) {
    LOG(INFO) << typename_ << " default construction, init nothing " << this;
  }
  NeighborTable(uint64_t initial_size)
      : own_purge_(true) {
    purge_ = new Purge();
    block_ = new NeighborBlock<V, Purge>(initial_size, purge_);
    LOG(INFO) << typename_ << " construction with initial bucket_size: " << block_->bucket_size_
              << " block: " << block_;
  }
  NeighborTable(uint64_t initial_size, Purge* purge, bool own_purge = false)
      : purge_(purge),
        own_purge_(own_purge),
        block_(new NeighborBlock<V, Purge>(initial_size, purge_)) {
    LOG(INFO) << typename_ << " construction with initial bucket_size: " << block_->bucket_size_ << " block: " << block_
              << " outter purge: " << purge_;
  }
  ~NeighborTable() {
    if (block_ == nullptr) {
      LITE_LOG(INFO) << typename_ << " block nullptr, maybe not inited";
      return;
    }
    LITE_LOG(INFO) << typename_ << " destruction block_size: " << block_->bucket_size_ << " block: " << block_
                   << " purge: " << purge_;

    block_->FlushKickoff();
    for (uint64_t i = 0; i < block_->bucket_size_; i++) {
      if (block_->buckets_[i].HasValue()) {
        purge_->NodeExpire((V*) (block_->buckets_[i].PointerAddr()));
      }
    }
    LOG(INFO) << "delete block " << block_;

    delete block_;
    if (own_purge_) {
      LOG(INFO) << typename_ << " own block and purge and delete purge: " << purge_;
      delete purge_;
    }
  }

  // 自己通过操作插入用
  void Init(uint64_t initial_size, Purge* purge = nullptr, bool own_purge = true) {
    if (!purge) {
      purge_ = new Purge();
    } else {
      purge_ = purge;
    }
    own_purge_ = own_purge;
    block_ = new NeighborBlock<V, Purge>(ceil((double) initial_size / FLAGS_hash_refactor), purge_);
    LITE_LOG(INFO) << typename_ << " Init, initial_size: " << initial_size << " bucket_size: " << block_->bucket_size_
                   << " purge: " << purge_;
  }

  // 直接从文件拷贝用
  bool InitForLoad(uint64_t bucket_size, Purge* purge = nullptr, bool own_purge = true) {
    if (!IsPowerOfTwo(bucket_size)) {
      LITE_LOG(WARNING) << typename_ << " bucket_size must be power of two";
      return false;
    }

    purge_ = purge != nullptr ? purge : new Purge();
    own_purge_ = own_purge;
    block_ = new NeighborBlock<V, Purge>(bucket_size, purge_);
    LITE_LOG(INFO) << typename_ << " LoadTable, bucket_size: " << bucket_size << " purge: " << purge_;
    return true;
  }

  void ReadTable(std::string& data) {
    Flush();
    block_->ReadBlock(data);
  }

  const V* Seek(const std::string& id) const {
    return block_->Seek(hash_(id));
  }

  const V* Seek(uint64_t id) const {
    return block_->Seek(hash_(id));
  }

  void Traverse(std::vector<uint64_t>& keys, std::vector<const V*>& values) const {
    return block_->Traverse(keys, values);
  }

  // 读安全的
  // 插入一条数据,如果需要挪位置会先将对应数据暂存起来,等到满足一等条件时在同步插到合适的位置
  // 等同于table帮忙做了特定条件的batch_upsert
  V* Upsert(uint64_t id, V* value) {
    return UpsertImpl_(hash_(id), value, true);
  }

  V* Upsert(const std::string& id, V* value) {
    return UpsertImpl_(hash_(id), value, true);
  }

  // 读安全的
  // 一次处理一批数据,统一算好位置后与读同步后更新对应位置
  void BatchUpsert(const std::vector<std::pair<uint64_t, V*>>& insert_values) {
    // LOG(INFO) << "batch insert, size: " << insert_values.size();
    for (auto& p: insert_values) {
      UpsertImpl_(hash_(p.first), p.second, false);
      // LOG(INFO) << "insert key: " << p.first << " value: " << p.second << " block size: " << block_->Size();
    }
    block_->FlushKickoff();
    // LOG(DEBUG) << "delay batch insert done, insert_values size: " << insert_values.size() << " current block size: " << block_->Size();
  }

  void Flush() {
    block_->FlushKickoff();
  }

  uint64_t Size() const {
    return block_->Size();
  }

  uint64_t BucketSize() const {
    return block_->BucketSize();
  }

  Purge* purge() {
    return purge_;
  }

  void KickUpsert(uint64_t id, V* value) {
    KickUpsert_(hash_(id), value);
  }

  void KickUpsert(const std::string& id, V* value) {
    KickUpsert_(hash_(id), value);
  }

  template <typename Visitor>
  void ForEach(const Visitor& visitor) {
    block_->ForEach(visitor);
  }

  NeighborNode* CurrentBuckets() {
    return block_->buckets_.get();
  }

  NeighborBlock<V, Purge>* neighbor_block() {
    return block_;
  }

 private:
  // 非读安全的,需要保证调用时没有读操作
  // 插入并且立马挪位置,当确定没有读操作时,可以使用此api,会比Upsert快点
  void KickUpsert_(uint64_t key, V* value) {
    if (!value) {
      LOG(ERROR) << "nullptr value is not supported, id: " << key << " value:" << value;
      return;
    }

    ExpandWhenBlockFull_();
    try {
      block_->KickUpsert(key, value);
    } catch (const std::exception& e) {
      LITE_LOG(FATAL) << typename_ << " fail to kick_upsert value, e.what(): " << e.what();
    }
  }

  V* UpsertImpl_(uint64_t key, V* value, bool auto_flush) {
    if (!value) {
      LOG(ERROR) << "nullptr value is not supported, key: " << key << " value:" << value;
      return nullptr;
    }
    // LOG(INFO) << "upsert id: " << id << " hash_id: " << hash_(id) << " value: " << (void*)value;
    ExpandWhenBlockFull_();
    try {
      return block_->Upsert(key, value, auto_flush);
    } catch (const std::exception& e) {
      LITE_LOG(FATAL) << typename_ << " fail to upsert value, e.what(): " << e.what();
    }
    return nullptr;
  }

  void ExpandWhenBlockFull_() {
    if (block_->Full()) {
      auto* old_block = block_;
      auto* new_block = new NeighborBlock<V, Purge>(block_->bucket_size_ * 2, purge_);
      LITE_LOG(INFO) << typename_ << " old block " << old_block << " is full, size: " << old_block->size_
                     << " expand from " << old_block->bucket_size_ << " to " << new_block->bucket_size_
                     << " new_block: " << new_block;

      new_block->ExpandFrom(block_);
      block_ = new_block;

      auto* gc = butil::get_object<BlockCollectType>();
      gc->pointer = old_block;
      purge_->epoch()->RetireUntilDone(std::move(gc));
    }
  }

  Purge* purge_{nullptr};
  Hash hash_;
  bool own_purge_;
  NeighborBlock<V, Purge>* block_{nullptr};
};

// 存48位数字的map,注意值如果是0的话,需要特殊处理
// 注意,插入的数据一定要是48位的
using NumeralNeighborTable = NeighborTable<uint64_t, NumberPurge>;

} // namespace index_engine
