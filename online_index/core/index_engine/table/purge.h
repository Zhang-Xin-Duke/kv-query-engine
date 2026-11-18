#pragma once
#include "index_engine/util/allocator.h"
#include "index_engine/util/epoch_controller.h"
#include "index_engine/util/table_util.h"
#if __cplusplus >= 202002L
#include <concepts>
#endif

namespace index_engine {

#if __cplusplus >= 202002L
template <typename T>
concept EpochConcept = requires(T epoch, uint64_t version) {
  { epoch.current_version() } -> std::convertible_to<uint64_t>;
  { epoch.BumpVersionUntilDone() } -> std::same_as<void>;
  { epoch.AllReleasedBeforeVersion(version) } -> std::convertible_to<bool>;
  { epoch.TryRelease() } -> std::same_as<void>;
  { epoch.Acquire() } -> std::convertible_to<uint64_t>;
  { epoch.Release(version) } -> std::same_as<void>;
};

template <typename T>
concept PurgeConcept = requires(
    T a,
    typename T::NodeType* pointer,
    std::vector<typename T::NodeType*>& retire_values,
    uint64_t version,
    uint64_t handler
) {
  typename T::Epoch;
  requires EpochConcept<typename T::Epoch>;

  typename T::NodeType;

  { a.NodeExpire(pointer) } -> std::same_as<void>;
  { a.WaitAllReadReleased() } -> std::same_as<void>;
  { a.RetireValues(retire_values) } -> std::same_as<void>; // Note this is pure virtual in the base
  { a.WaitReadRelease(version) } -> std::same_as<void>;
  { a.Acquire() } -> std::convertible_to<uint64_t>;
  { a.Release(handler) } -> std::same_as<void>;
  { a.epoch() } -> std::convertible_to<typename T::Epoch*>;
};

#endif

template <typename T>
class PointerCollector : public base::GarbageCollector {
 public:
  PointerCollector() {}

  void Recycle() override {
    delete pointer;
    pointer = nullptr;
    butil::return_object(this);
  }

  T* pointer{nullptr};
};

#if __cplusplus >= 202002L
template <typename V, PurgeConcept Purge>
#else
template <typename V, typename Purge>
#endif
class NeighborBlock;

// max waiting pending element = kDefaultPendingSize * kMaxRetireSize
ALLOW_UNUSED const static int kDefaultPendingSize = 32 * 1024;
ALLOW_UNUSED const static int kMaxRetireSize = 1024;
ALLOW_UNUSED const static int kMaxRetireIntervalMills = 60 * 1000;
ALLOW_UNUSED const static int kDefaultChunkSize = 4096;

template <typename V>
class PurgeBase {
 public:
  using Epoch = base::PolymorphismEpochController;
  using NodeType = V;
  PurgeBase()
      : epoch_(kDefaultPendingSize, base::GarbageCollectorRecycler()) {}
  virtual ~PurgeBase() {};

  void NodeExpire(V* pointer) {
    // LOG(INFO) << "retire pointer: " << (void*)pointer;
    retired_values_.push_back(pointer);
    auto now_ms = butil::gettimeofday_ms();
    if ((int) retired_values_.size() >= kMaxRetireSize
        || now_ms - last_retire_timestamp_ms_ > kMaxRetireIntervalMills) {
      RetireValues(retired_values_);
      retired_values_.clear();
      last_retire_timestamp_ms_ = now_ms;
    }
  }

  void WaitAllReadReleased() {
    uint64_t read_version = epoch_.current_version();
    epoch_.BumpVersionUntilDone();
    WaitReadRelease(read_version);
  }

  virtual void RetireValues(std::vector<V*>& retire_values) = 0;

  void WaitReadRelease(uint64_t version) {
    while (!epoch_.AllReleasedBeforeVersion(version)) {
      LOG_BY_INTERVAL(INFO, 10) << "wait for all read complete,"
                                << " min_slot version: " << epoch_.min_pending_version() << " version: " << version
                                << " epoch version: " << epoch_.current_version();
      epoch_.TryRelease();
      usleep(10 * 1000);
    }
  }

  uint64_t Acquire() {
    uint64_t handler = epoch_.Acquire();
    return handler;
  }

  void Release(uint64_t handler) {
    epoch_.Release(handler);
  }

  Epoch* epoch() {
    return &epoch_;
  }

 protected:
  Epoch epoch_;
  std::vector<V*> retired_values_;
  int64_t last_retire_timestamp_ms_{butil::gettimeofday_ms()};
};

template <typename V, typename Allocator = ChunkAllocator<V>>
class AllocatorPurge : public PurgeBase<V> {
 public:
  AllocatorPurge()
      : PurgeBase<V>() {
    // 4096
    allocator_.Init(kDefaultChunkSize);
  }
  ~AllocatorPurge() {
    LITE_LOG(INFO) << "destruction of AllocatorPurge: " << this;
    RetireValues(this->retired_values_);
    this->WaitAllReadReleased();
  }

  void RetireValues(std::vector<V*>& retire_values) override {
    // LOG_BY_INTERVAL(INFO, 1) << "batch retire values, retired_values size: " << retire_values.size();
    this->epoch_.RetireUntilDone(BatchValueGarbageCollector<V, Allocator>::Build(retire_values, &allocator_));
  }

  Allocator* allocator() {
    return &allocator_;
  }

 private:
  Allocator allocator_;
};

template <typename V>
using ChunkPurge = AllocatorPurge<V, ChunkAllocator<V>>;

/**
 * 直接保存48位数字的purge,回收的时候啥也不干
 */
class NumberPurge : public PurgeBase<uint64_t> {
 public:
  NumberPurge()
      : PurgeBase<uint64_t>() {
    LITE_LOG(INFO) << "construction of NumberPurge: " << this;
  }
  ~NumberPurge() {
    LITE_LOG(INFO) << "destruction of NumberPurge" << this;
  }

  // rawvalue, nothing need to do
  void RetireValues(std::vector<uint64_t*>& /** retire_values */) override {}
};

} // namespace index_engine
