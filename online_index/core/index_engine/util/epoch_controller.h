#pragma once

#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "index_engine/util/logging.h"

namespace index_engine {
namespace base {

// 生命周期控制器，思路类似于HazzardPointer
// 适用于需要一写多读且无锁的场景，例如一个线程更新数据，其他线程访问数据
// 一写多读的无锁容器的实现可以根据业务场景自行实现，这里不涉及
// 这里解决的是一写多读场景下的生命周期问题
// 例如：我们很容易实现一个全局变量的一写多读，假如是一个map，如下
// std::map<std::string, int>* g_map;
// 使用上，比如一个线程定期从文件更新这个g_map，更新的方式就是全量替换，比如
// auto* new_map = new std::map<std::string, int>;
// for (auto item : ReadLinesFromFile<std::string, int>("./data.txt")) {
//   new_map->emplace(item);
// }
// g_map = new_map;
// 而很多线程会读这个map，比如：
// auto it = g_map->find("my_config");
// int my_config = it != g_map->end() ? it->second : 0;
// 显然，以上实现是安全的，但是存在内存泄漏，旧的map并没有释放
// 而如果我们更新了g_map之后就立刻释放原来的map，又会存在安全性问题
// 读线程可能已经读到了旧的map，正在调用find，或者后续的iterator方法
// 解决这个问题，有几种方法
// 1. doubly buffer，过一段安全的时间(10s)再释放，
//    这种方法基于一个假定：读线程肯定会在这个安全的时间内完成对资源的使用
// 2. 加强版doubly buffer，把这个g_map改成std::shared_ptr<std::map<...>>，
//    shared_ptr可以保证安全性，但是shared_ptr并没有低成本的atomic方案
//    也就是说读写这个shared_ptr，是需要加锁或者其他机制(128位的atomic其实很慢)来保证的
// 可见，生命周期管理是实现一写多读场景下的一个很棘手的点
// 具体来讲，就是 什么时候可以安全删除被删除的资源
// 因此，这里尝试提供一种更快，更低成本也更灵活的方式
// 再次强调，这里只关注资源的删除
// 核心思路如下：
// 1. 写线程删除资源的时候，这时被删除的资源应该(使用方保证)已经从访问路径中摘掉了，例如上述例子中的g_map已经指向了新的对象。
//    删除资源的时候，并不当场释放，而是将待释放资源放在一个容器中，这个容器使用方自定义即可，
//    当使用方认为需要发起一次资源回收的时候，这个时机可以是一有资源删除就触发，也可以是攒够一批，也可以是定时，由业务方自己决定。
//    总之，使用方认为需要发起回收的时候，用这个容器调用EpochController::Retire()，这会导致EpochController内部的版本号 + 1，
//    并在恰当的时机用这个容器调用Recycler
// 2. 读线程每次读资源之前，先调用Acquire拿到一个handle，这个时刻记作x，代表锁定了当前时刻的资源版本号
//    直到读线程Release这个handle之前，x时刻之后写线程retire掉的所有资源都不会被真正释放，对这些资源的访问都是有效的
//    Release handle之后，假如Controller发现不再有旧版本的资源锁定了，对应的资源就会被真正释放
// 上文中出现了资源的 删除，retire，释放，语义各不相同，注意分辨
// BumpVersion的调用时机是 需要发起回收 的时刻，而不是资源有写入的时刻
// 使用方可以定制RetireContainer和Recycler
// 例如：资源像上述一样的double-buffer类型的，RetireContainer只需要一个std::unique_ptr就够了，Recycler里直接reset掉
// 假如使用方是持续更新一个容器，可能会频繁删除其中的一些子资源（比如map里的key-value）
//     这种更适合把RetireContainer实现成一个vector或者linked-list，攒够一批一起释放
// Recycler里用户可以自定义回收逻辑，比如可以是归还给系统，也可以是归还给内存池，还可以归还之后给外部通知(用户自行实现)
template <class RetireContainer, class Recycler>
class EpochController {
 public:
  using Container = RetireContainer;

  EpochController(int max_pending_version_count, Recycler&& recycler) {
    max_pending_version_count_ = max_pending_version_count;
    pendings_.reset(new PendingVersion[max_pending_version_count_]);
    pendings_[0].version = 1;
    pendings_[0].refcnt.store(0, std::memory_order_relaxed);
    current_head_.store(0, std::memory_order_release);
    recycler_ = std::move(recycler);

    current_slot_ = 0;
    min_slot_ = max_pending_version_count_ - 1;
    pendings_[min_slot_].version = 0;
  }

  // READER should acquire a handle BEFORE read the protected resource
  uint64_t Acquire() {
    return current_head_.fetch_add(1, std::memory_order_acquire);
  }

  // READER should release the handle AFTER finish read
  void Release(uint64_t head) {
    //    LITE_LOG(INFO) << "Release " << head;
    uint32_t slot = read_slot(head);
    uint64_t current_head = current_head_.load(std::memory_order_relaxed);
    while (read_slot(current_head) == slot) {
      if (current_head_.compare_exchange_strong(current_head, current_head - 1, std::memory_order_acq_rel)) {
        return;
      }
    }

    int64_t prev = pendings_[slot].refcnt.fetch_sub(1, std::memory_order_release);
    if (read_refcount(prev) == 1) {
      SignalRecycle();
    }
  }

  bool PendingFull() {
    return pending_version_count() >= max_pending_version_count_;
  }

  // 有两种场景可以Bump:
  //  1. 有资源可以回收,需要BumpVersion后,当读请求都结束后,可以安全释放资源
  //  2. 有数据同步需求,需要保证之前的读操作都已完成,才能安全的进行某些操作;仅用于做数据同步,无需释放资源
  bool BumpVersion() {
    MaybeRecycle();
    if (PendingFull()) {
      LITE_LOG(WARNING) << "too much pending version,"
                        << " current: " << pending_version_count() << " max: " << max_pending_version_count_;
      return false;
    }

    // wrap-around
    current_slot_ = NextSlot(current_slot_);

    uint64_t new_head = (uint64_t(current_slot_) << 32);
    pendings_[current_slot_].refcnt.store(0, std::memory_order_relaxed);
    current_version_ += 1;
    pendings_[current_slot_].version = current_version_;

    uint64_t old_head = current_head_.exchange(new_head, std::memory_order_acq_rel);
    int64_t old_count = read_refcount(old_head);
    int64_t prev = pendings_[read_slot(old_head)].refcnt.fetch_add(old_count);
    if (prev + old_count == 0) {
      Recycle();
    }
    return true;
  }

  void BumpVersionUntilDone() {
    while (PendingFull()) {
      LITE_LOG(WARNING) << "bump version pending list is full, recycle and wait";
      Recycle();
      usleep(10 * 1000);
    }
    BumpVersion();
  }

  RetireContainer* current_retire_container() {
    return &pendings_[current_slot_].retired_resource;
  }

  bool Retire(RetireContainer&& container) {
    pendings_[current_slot_].retired_resource = std::move(container);
    pendings_[current_slot_].need_retire = true;
    return BumpVersion();
  }

  void RetireUntilDone(RetireContainer&& container) {
    while (PendingFull()) {
      // LITE_LOG(WARNING) << "pending list is full, recycle and wait";
      Recycle();
      usleep(10 * 1000);
    }
    Retire(std::move(container));
  }

  void MaybeRecycle() {
    int signal = recycle_singals_.exchange(0, std::memory_order_acquire);
    if (signal) {
      Recycle();
    }
  }

  // version added by bump
  bool AllReleasedBeforeVersion(uint64_t version) {
    /**
    LOG_BY_INTERVAL(INFO, 1) << "wait for version release, min_slot: " << min_slot_
                             << " min_slot version: " << pendings_[min_slot_].version
                             << " wait version: " << version
                             << " current version: " << current_version_;
    */
    return pendings_[min_slot_].version >= version;
  }

  int current_version() const {
    return current_version_;
  }

  int min_pending_version() const {
    return pendings_[min_slot_].version;
  }

  void TryRelease() {
    Recycle();
  }

 private:
  size_t pending_version_count() const {
    if (current_slot_ > min_slot_) {
      return current_slot_ - min_slot_;
    }
    return max_pending_version_count_ - (min_slot_ - current_slot_);
  }

  static uint32_t read_slot(uint64_t head) {
    return head >> 32;
  }

  static uint32_t read_refcount(uint64_t head) {
    return (head << 32) >> 32;
  }

  int NextSlot(int slot) {
    size_t next = slot + 1;
    return next >= max_pending_version_count_ ? 0 : next;
  }

  void SignalRecycle() {
    recycle_singals_.fetch_add(1, std::memory_order_release);
  }

  void Recycle() {
    std::vector<int> recycled_slots;
    int slot = NextSlot(min_slot_);
    while (slot != current_slot_) {
      if (pendings_[slot].refcnt.load(std::memory_order_acquire) != 0) {
        break;
      }

      recycled_slots.push_back(slot);
      slot = NextSlot(slot);
    }

    // recycle
    for (auto recycle_slot: recycled_slots) {
      if (pendings_[recycle_slot].need_retire) {
        recycler_(pendings_[recycle_slot].version, pendings_[recycle_slot].retired_resource);
        pendings_[recycle_slot].need_retire = false;
      }
    }

    if (!recycled_slots.empty()) {
      min_slot_ = recycled_slots.back();
    }
  }

  struct PendingVersion {
    bool need_retire{false};
    uint64_t version{};
    std::atomic<int64_t> refcnt{};
    RetireContainer retired_resource;
  };

  std::unique_ptr<PendingVersion[]> pendings_;
  size_t max_pending_version_count_;

  uint64_t current_version_ = 0;

  int current_slot_;
  int min_slot_;

  // version(32bit) | count(32bit)
  std::atomic<uint64_t> current_head_;

  std::atomic<int> recycle_singals_{0};

  Recycler recycler_;
};

class GarbageCollector {
 public:
  virtual ~GarbageCollector() {}
  virtual void Recycle() = 0;
};

class GarbageCollectorRecycler {
 public:
  void operator()(uint64_t, GarbageCollector* c) {
    c->Recycle();
  }
};

using PolymorphismEpochController = EpochController<GarbageCollector*, GarbageCollectorRecycler>;

} // namespace base
} // namespace index_engine
