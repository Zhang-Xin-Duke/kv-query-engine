#pragma once

#include <cstdint>
#include <memory>
#include <type_traits>

#include "bthread/bthread.h"
#include "index_engine/util/dir_based.h"
#include "index_engine/util/epoch_controller.h"
#include "index_engine/util/memory_return.h"
#include "index_engine/util/logging.h"
#include "index_engine/util/table_util.h"

namespace index_engine {

namespace internal {
template <class T, bool NeedEpochController>
struct ShardCreator {
  static std::unique_ptr<T> Create(base::PolymorphismEpochController*) {
    return std::make_unique<T>();
  }
};

template <class T>
struct ShardCreator<T, true> {
  static std::unique_ptr<T> Create(base::PolymorphismEpochController* epoch) {
    return std::make_unique<T>(epoch);
  }
};
} // namespace internal

template <class T>
class ShardedReloadable {
 private:
  DEFINE_TYPE_NAME(T);

 public:
  using ReloadableTable = T;
  class GarbageCollector : public base::GarbageCollector {
   public:
    void Recycle() override {
      LITE_LOG(INFO) << "recycle table shard:" << shard;
      delete shard;
      if (has_been_recycled) {
        *has_been_recycled = true;
      }
      delete this;
    }

    T* shard{nullptr};
    bool* has_been_recycled{nullptr};
  };
  struct ShardInitializer {
    int index;
    ShardedReloadable<T>* sharded_reloadable;
    const ShardMeta* shard_meta;
    std::atomic<uint64_t>* initialized_count;
    std::atomic<bool>* no_error_occured;
  };

  ShardedReloadable() {}
  ~ShardedReloadable() {
    for (int i = 0; i < shard_count_; ++i) delete shards_[i].load();
  }

  ShardedReloadable(const std::string& dir, base::PolymorphismEpochController* epoch)
      : dir_(dir),
        epoch_(epoch) {}

  void InitDirEpoch(const std::string& dir, base::PolymorphismEpochController* epoch) {
    dir_.set_dir(dir);
    epoch_ = epoch;
  }

  void ResetDir(const std::string& dir) {
    dir_.set_dir(dir);
  }

  // Configure before serving; the query path does not inspect this policy.
  void set_memory_return_policy(MemoryReturnPolicy policy, size_t every = 1) {
    memory_return_.Configure(policy, every);
  }

  int ShardIndex(uint64_t key) {
    return (key >> 48) & (shard_count_ - 1);
  }

  T* GetShard(uint64_t key) {
    return shards_[ShardIndex(key)].load(std::memory_order_acquire);
  }

  bool Init(const SnapshotMeta& meta) {
    return Load(meta);
  }

  using ShardCreator = internal::ShardCreator<T, std::is_constructible<T, base::PolymorphismEpochController*>::value>;

  static void* LoadShard(void* args) {
    auto* shard_initializer = (ShardInitializer*) args;
    auto* sharded = shard_initializer->sharded_reloadable;

    butil::Timer timer{butil::Timer::STARTED};
    auto& shard_info = *shard_initializer->shard_meta;
    LITE_LOG(INFO) << sharded->typename_ << " start to load shard: " << ProtoStringify(shard_info);
    auto shard = ShardCreator::Create(sharded->epoch_);
    if (sharded->on_shard_create_) {
      sharded->on_shard_create_(shard.get());
    }
    if (shard->Load(sharded->dir_, shard_info)) {
      sharded->shards_[shard_initializer->index] = shard.release();
    } else {
      LITE_LOG(ERROR) << sharded->typename_ << " fail to load shard:" << shard_info.dir_name();
      *shard_initializer->no_error_occured = false;
    }

    timer.stop();
    LITE_LOG(INFO) << sharded->typename_ << " finish load shard " << shard_info.dir_name()
                   << " items_total_count: " << shard_info.items_total_count() << " shard cost: " << timer.m_elapsed()
                   << " ms";
    shard_initializer->initialized_count->fetch_add(1);
    // Load may exit here, everything should be safe
    delete shard_initializer;
    return nullptr;
  }

  bool Load(const SnapshotMeta& meta) {
    butil::Timer load_timer{butil::Timer::STARTED};
    LITE_LOG(INFO) << typename_ << " start to load snapshot:" << dir_.dir()
                   << " meta partitions:" << StringifyPartitions(meta.partitions())
                   << " items_total_count: " << meta.items_total_count() << " timestamp: " << meta.timestamp();
    const auto& shard_list = meta.shard_info_list();
    shard_count_ = shard_list.size();
    if (shard_count_ == 0 || (shard_count_ & (shard_count_ - 1)) != 0) {
      LITE_LOG(ERROR) << typename_ << " invalid shard_count, it should be power of 2 " << shard_count_;
      shard_count_ = 0;
      return false;
    }

    shards_.reset(new std::atomic<T*>[shard_count_]{});
    std::atomic<uint64_t> initialized_count{0};
    std::atomic<bool> no_error_occured{true};
    for (int i = 0; i < shard_count_; i++) {
      auto* shard_initializer = new ShardInitializer;
      shard_initializer->index = i;
      shard_initializer->sharded_reloadable = this;
      shard_initializer->shard_meta = &shard_list[i];
      shard_initializer->initialized_count = &initialized_count;
      shard_initializer->no_error_occured = &no_error_occured;

      bthread_t tid;
      if (bthread_start_background(&tid, nullptr, &ShardedReloadable<T>::LoadShard, shard_initializer) != 0) {
        CHECK(false) << "fail to start load shard bthread";
        return false;
      }
    }
    butil::Timer timer{butil::Timer::STARTED};
    while (initialized_count.load(std::memory_order_acquire) != (uint64_t) shard_count_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      timer.stop();
      LOG_BY_INTERVAL(INFO, 5) << typename_ << " wait for shard load shard finished, "
                               << " wait for: " << timer.m_elapsed() << " ms";
    }

    load_timestamp_ = meta.timestamp();
    load_timer.stop();
    LITE_LOG(INFO) << typename_ << " finish load snapshot, dir: " << dir_.dir()
                   << " items_total_count: " << meta.items_total_count() << " meta_timestamp: " << meta.timestamp()
                   << " success: " << (no_error_occured ? "true" : "false") << " cost: " << load_timer.m_elapsed()
                   << " ms";
    return no_error_occured;
  }

  template <class OnShardLoad>
  bool Reload(const SnapshotMeta& meta, const OnShardLoad& on_shard_load) {
    butil::Timer timer{butil::Timer::STARTED};
    LITE_LOG(INFO) << typename_ << " start to reload snapshot, dir: " << dir_.dir()
                   << " shards_count: " << meta.shard_info_list().size()
                   << " items_total_count:" << meta.items_total_count()
                   << " meta partitions:" << StringifyPartitions(meta.partitions())
                   << " timestamp: " << meta.timestamp();
    const auto& shard_list = meta.shard_info_list();
    int shard_count = shard_list.size();
    if (shard_count != shard_count_) {
      LITE_LOG(ERROR) << typename_ << " invalid shard count: " << shard_count << " " << shard_count_;
      return false;
    }

    for (int shard_index = 0; shard_index < shard_count_; ++shard_index) {
      butil::Timer shard_timer{butil::Timer::STARTED};
      auto const& shard_info = shard_list[shard_index];
      LITE_LOG(INFO) << typename_ << " start to load shard "
                     << " shard_index: " << shard_index << " shard_meta: " << ProtoStringify(shard_info);

      auto new_shard = ShardCreator::Create(epoch_);
      if (this->on_shard_create_) {
        this->on_shard_create_(new_shard.get());
      }
      /**
      if (!new_shard->Load(dir_, shard_info)) {
        LITE_LOG(WARNING) << shard_type_name_ << " fail to reload table shard:" << shard_index;
        return false;
      }
       */
      on_shard_load(shards_.get(), shard_index, new_shard.release(), dir_.dir(), shard_info);
      shard_timer.stop();
      LITE_LOG(INFO) << typename_ << " finish reload shard table "
                     << " shard_index: " << shard_index << " shard_meta: " << ProtoStringify(shard_info)
                     << " shard reload cost: " << shard_timer.m_elapsed() << " ms";
    }
    timer.stop();
    LITE_LOG(INFO) << typename_ << " success to reload snapshot,"
                   << " shard_count:" << shard_count_ << " snapshot reload cost: " << timer.m_elapsed() << " ms";
    load_timestamp_ = meta.timestamp();
    return true;
  }

  bool Reload(const SnapshotMeta& meta) {
    return Reload(
        meta,
        [this](std::atomic<T*>*, int shard_index, T* new_shard, const std::string& dir, const ShardMeta& shard_meta) {
          if (!new_shard->Load(DirBased(dir), shard_meta)) {
            LITE_LOG(WARNING) << "fail to load shard, shard_meta: " << ProtoStringify(shard_meta);
            delete new_shard;
            throw std::runtime_error("shard reload failed");
          }
          UpdateShard(shard_index, new_shard);
        }
    );
  }

  void UpdateShard(int shard_index, T* new_shard) {
    // The caller finished loading the replacement; temporary builders are gone.
    if (memory_return_.ReplacementReady()) ReturnUnusedMemory(MemoryReturnPolicy::Trim);
    auto* old_shard = shards_[shard_index].exchange(new_shard, std::memory_order_acq_rel);

    butil::Timer timer{butil::Timer::STARTED};
    LITE_LOG(INFO) << "start to recycle table shard:" << old_shard;
    bool has_been_recycled = false;
    auto* gc = new GarbageCollector;
    gc->shard = old_shard;
    gc->has_been_recycled = &has_been_recycled;
    epoch_->RetireUntilDone(gc);
    while (!has_been_recycled) {
      epoch_->TryRelease();
      // 10ms
      bthread_usleep(10000);
    }
    // Epoch reclamation completed before requesting physical page return.
    if (memory_return_.Reclaimed(shard_index + 1 == shard_count_))
      ReturnUnusedMemory(MemoryReturnPolicy::Trim);
    LITE_LOG(INFO) << "shard recycled:" << old_shard << " update shard index " << shard_index << " cost "
                   << timer.m_elapsed() << " ms";
  }

  void GetShards(std::vector<T*>& shards) {
    for (int i = 0; i < shard_count_; ++i) {
      shards.push_back(shards_[i]);
    }
  }

  template <class Visitor>
  void ForEach(const Visitor& visitor) {
    for (int i = 0; i < shard_count_; ++i) {
      shards_[i].load()->Visit(visitor);
    }
  }

  template <typename TableVisitor>
  void ForEachTable(const TableVisitor& table_visitor) {
    for (int i = 0; i < shard_count_; i++) {
      table_visitor(shards_[i]);
    }
  }

  void set_on_shard_create(std::function<void(T*)>&& on_shard_create) {
    on_shard_create_ = std::move(on_shard_create);
  }

 private:
  DirBased dir_;
  MemoryReturnSchedule memory_return_;
  std::unique_ptr<std::atomic<T*>[]> shards_;
  int shard_count_{0};
  base::PolymorphismEpochController* epoch_{nullptr};
  uint64_t load_timestamp_;
  std::function<void(T*)> on_shard_create_;
};

template <class T, class EP>
class BaseEpochProtectedPtr {
 public:
  BaseEpochProtectedPtr(const BaseEpochProtectedPtr&) = delete;
  BaseEpochProtectedPtr& operator=(const BaseEpochProtectedPtr&) = delete;

  BaseEpochProtectedPtr(BaseEpochProtectedPtr&& ptr) {
    ptr_ = ptr.ptr_;
    handle_ = ptr.handle_;
    epoch_ = ptr.epoch_;
    ptr.ptr_ = nullptr;
  }

  BaseEpochProtectedPtr& operator=(BaseEpochProtectedPtr&& ptr) {
    ptr_ = ptr.ptr_;
    handle_ = ptr.handle_;
    epoch_ = ptr.epoch_;
    ptr.ptr_ = nullptr;
    return *this;
  }

  T* operator->() {
    return ptr_;
  }

  const T* operator->() const {
    return ptr_;
  }

  ~BaseEpochProtectedPtr() {
    if (ptr_) {
      epoch_->Release(handle_);
    }
  }

  BaseEpochProtectedPtr(T* ptr, uint64_t handle, EP* epoch)
      : ptr_(ptr),
        handle_(handle),
        epoch_(epoch) {}

 private:
  T* ptr_{nullptr};
  uint64_t handle_;
  EP* epoch_;
};

template <class T>
class ReloadableResource {
 public:
  struct RetiredContainer {
    std::unique_ptr<T> resource;
    bool* has_been_recycled;
  };

  struct Recycler {
    void operator()(uint64_t, RetiredContainer& resource) {
      resource.resource.reset();
      *resource.has_been_recycled = true;
    }
  };

  using Epoch = base::EpochController<RetiredContainer, Recycler>;

  ReloadableResource()
      : epoch_(2, Recycler()) {}

  using ProtectedPtr = BaseEpochProtectedPtr<T, Epoch>;

  ProtectedPtr GetProtected();
  void Update(T* resource);
  void Init(T* resource) {
    resource_ = resource;
  }

 private:
  T* resource_{nullptr};
  Epoch epoch_;
};

template <class T>
typename ReloadableResource<T>::ProtectedPtr ReloadableResource<T>::GetProtected() {
  return ProtectedPtr(resource_, epoch_.Acquire(), &epoch_);
}

template <class T>
void ReloadableResource<T>::Update(T* resource) {
  RetiredContainer c;
  c.resource.reset(resource_);
  resource_ = resource;
  bool has_been_recycled = false;
  c.has_been_recycled = &has_been_recycled;
  epoch_.RetireUntilDone(std::move(c));
  while (!has_been_recycled) {
    epoch_.TryRelease();
    // 10ms
    bthread_usleep(10000);
  }
}

} // namespace index_engine
