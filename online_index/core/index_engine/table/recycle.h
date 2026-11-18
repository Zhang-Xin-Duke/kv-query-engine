#pragma once
#include <condition_variable>
#include <memory>
#include <mutex>

#include "index_engine/util/logging.h"

namespace index_engine {

template <typename T>
struct WaitForContainer {
  std::unique_ptr<T> resource;
  bool* finished;
};

template <typename T>
struct WaitForRecycle {
  void operator()(uint64_t version, WaitForContainer<T>& retired_resource) {
    LITE_LOG(INFO) << "recycle table " << retired_resource.resource.get();
    retired_resource.resource.reset();
    *retired_resource.finished = true;
  }
};

struct Notifier {
  std::mutex mu;
  std::condition_variable cv;
  bool recycled{false};
};

/**
 * 前一个数据释放时,可以通过notifier将事件通知出来
 * @tparam T
 */
template <typename T>
struct NotifiableRawPtrContainer {
  NotifiableRawPtrContainer() {}
  NotifiableRawPtrContainer(T* resource_ptr, Notifier* notifier_ptr)
      : resource(std::unique_ptr<T>(resource_ptr)),
        notifier(notifier_ptr) {}
  std::unique_ptr<T> resource;
  Notifier* notifier{nullptr};
};

template <typename T>
struct NotifiableRawPtrRecycle {
  void operator()(uint64_t version, NotifiableRawPtrContainer<T>& retired_resource) {
    retired_resource.resource.reset();
    if (retired_resource.notifier) {
      LITE_LOG(INFO) << "recycle table " << retired_resource.resource.get()
                     << " notifier: " << retired_resource.notifier;
      std::unique_lock<std::mutex> lock(retired_resource.notifier->mu);
      retired_resource.notifier->recycled = true;
      retired_resource.notifier->cv.notify_one();
    }
  }
};

template <typename Allocator, typename Pointer, typename Table>
struct AllocatorContainer {
  Allocator* allocator{nullptr};
  Pointer* pointer{nullptr};
  std::unique_ptr<NotifiableRawPtrContainer<Table>> table_container;
};

template <typename Allocator, typename Pointer, typename Table>
struct GroupAllocatorContainer {
  Allocator* allocator{nullptr};
  std::vector<Pointer*> pointer_list;
  std::unique_ptr<NotifiableRawPtrContainer<Table>> table_container;
};

template <typename Allocator, typename Pointer, typename Table>
struct AllocatorRecycle {
  void operator()(uint64_t version, AllocatorContainer<Allocator, Pointer, Table>& retired_resource) {
    // recycle element
    if (retired_resource.pointer) {
      // VLOG(8) << "recycle release element: " << (void*) retired_resource.pointer;
      // LITE_LOG(INFO) << "recycle release element: " << (void*) retired_resource.pointer;
      retired_resource.allocator->Release(retired_resource.pointer);
      retired_resource.pointer = nullptr;
      return;
    }
    // recycle table and notify
    if (retired_resource.table_container && retired_resource.table_container->resource) {
      LITE_LOG(INFO) << "release table: " << retired_resource.table_container->resource.get()
                     << " notify: " << retired_resource.table_container->notifier;
      retired_resource.table_container->resource->Stop();
      retired_resource.table_container->resource.reset();
      if (retired_resource.table_container->notifier) {
        std::unique_lock<std::mutex> lock(retired_resource.table_container->notifier->mu);
        retired_resource.table_container->notifier->recycled = true;
        retired_resource.table_container->notifier->cv.notify_one();
      }
    }
  }
};

template <typename Allocator, typename Pointer, typename Table>
struct GroupAllocatorRecycle {
  void operator()(uint64_t version, GroupAllocatorContainer<Allocator, Pointer, Table>& retired_resource) {
    // recycle element
    if (retired_resource.pointer_list.size() > 0) {
      VLOG(8) << "recycle release elements, size: " << retired_resource.pointer_list.size();
      for (auto& pointer: retired_resource.pointer_list) {
        retired_resource.allocator->Release(pointer);
      }
      retired_resource.pointer_list.clear();
      return;
    }
    // recycle table and notify
    if (retired_resource.table_container && retired_resource.table_container->resource) {
      LITE_LOG(INFO) << "release table: " << retired_resource.table_container->resource.get();
      retired_resource.table_container->resource->Stop();
      retired_resource.table_container->resource.reset();
      if (retired_resource.table_container->notifier) {
        std::unique_lock<std::mutex> lock(retired_resource.table_container->notifier->mu);
        retired_resource.table_container->notifier->recycled = true;
        retired_resource.table_container->notifier->cv.notify_one();
      }
    }
  }
};

} // namespace index_engine
