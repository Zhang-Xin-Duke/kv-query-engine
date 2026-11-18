#pragma once

#include <iostream>
#include <memory>

#if __cplusplus >= 202002L
#include <concepts>
#endif

#include "bthread/mutex.h"
#include "butil/string_printf.h"
#include "butil/strings/string_piece.h"
#include "index_engine/util/logging.h"
#include "index_engine/util/macros.h"
#include "index_engine/util/table_util.h"

namespace index_engine {

template <typename T>
class DefaultAllocator {
 public:
  //  explicit DefaultAllocator(size_t = 4096) {
  //  }
  void Init(size_t, size_t = sizeof(T)) {}
  //  void PreAllocator(int) {
  //  }
  template <class... ARGS>
  T* Acquire(ARGS&&... args) {
    return new T(std::forward<ARGS>(args)...);
  }

  template <class... ARGS>
  void MutexAcquireBatch(std::vector<T*>& values, int n, ARGS&&... args) {
    for (int i = 0; i < n; i++) {
      auto&& pointer = Acquire(std::forward<ARGS>(args)...);
      values[i] = pointer;
    }
  }

  void Release(T* x) {
    delete x;
  }
};

template <class T>
class ChunkAllocator {
 public:
  explicit ChunkAllocator(size_t chunk_size = 4096) {
    chunk_size_ = chunk_size;
    chunks_ = NULL;
    free_nodes_ = NULL;
    count_ = 0;
    typename_ = util::type_name<T>();
  }

  ~ChunkAllocator() {
    while (chunks_ != NULL) {
      Chunk* x = chunks_;
      chunks_ = chunks_->next;
      free(x);
    }
  }

  void Init(size_t chunk_size, size_t node_size = sizeof(T)) {
    chunk_size_ = chunk_size;
    const auto alignment = std::max(alignof(T), alignof(void*));
    node_size_ = (std::max(node_size, sizeof(void*)) + alignment - 1) / alignment * alignment;
  }

  void PreAllocator(int element_count) {
    // chunk_size never 0
    int chunk_count = (int) std::ceil((float) element_count / chunk_size_);
    for (int i = 0; i < chunk_count; i++) {
      CreateNewChunk();
    }
    LITE_LOG(INFO) << typename_ << " pre allocate with elements: " << element_count << " chunks: " << chunk_count
                   << " enough for " << chunk_count * chunk_size_ << " elements";
  }

  template <class... ARGS>
  T* Acquire(ARGS&&... args) {
    if (free_nodes_ == NULL) {
      CreateNewChunk();
    }
    T* x = (T*) (void*) free_nodes_;
    free_nodes_ = free_nodes_->next;
    new (x) T(std::forward<ARGS>(args)...);
    count_ += 1;
    return x;
  }

  template <class... ARGS>
  void MutexAcquireBatch(std::vector<T*>& values, int n, ARGS&&... args) {
    std::lock_guard<bthread::Mutex> guard(mutex_);
    for (int i = 0; i < n; i++) {
      auto&& pointer = Acquire(std::forward<ARGS>(args)...);
      values[i] = pointer;
    }
  }

  void Release(T* x) {
    x->~T();
    released_count_++;
    LOG_BY_INTERVAL(INFO, 30) << typename_ << " Release Chunk(" << this << ") "
                              << ", memory size: " << chunk_size_ * 32 << ", current count: " << count_
                              << ", released count: " << released_count_;
    Node* node = (Node*) (void*) x;
    node->next = free_nodes_;
    free_nodes_ = node;
    count_ -= 1;
  }

  size_t Size() const {
    return count_;
  }

 private:
  struct Node {
    union {
      Node* next;
      char data[sizeof(T)];
    };
  };

  struct Chunk {
    Chunk* next;
    Node nodes[0];
  };

  size_t node_size_{std::max(sizeof(T), sizeof(void*))};       // one node size
  size_t chunk_size_;      // how many elements in one chunk
  Chunk* chunks_{nullptr}; // a list
  Node* free_nodes_;       // a list
  size_t count_;           // how many elements are acquired
  size_t chunk_count_ = 0;
  size_t released_count_ = 0;
  bthread::Mutex mutex_;
  std::string typename_;

  void CreateNewChunk() {
    Chunk* chunk;
    const size_t alignment = std::max(alignof(T), alignof(void*));
    const size_t header_size = (sizeof(Chunk) + alignment - 1) / alignment * alignment;
    size_t size = header_size + node_size_ * chunk_size_;
    PCHECK(0 == posix_memalign((void**) &chunk, alignment, size));
    chunk->next = chunks_;
    chunks_ = chunk;

    char* nodes_mem = reinterpret_cast<char*>(chunk) + header_size;
    for (size_t i = 0; i < chunk_size_; i++) {
      Node* node = (Node*) (nodes_mem + i * node_size_);
      node->next = free_nodes_;
      free_nodes_ = node;
    }

    chunk_count_ += 1;
    LOG_BY_INTERVAL(INFO, 1) << typename_ << " Create Chunk(" << this << ")"
                             << ", memory size: " << size * 32 << ", current count: " << count_
                             << ", released count: " << released_count_;
  }
};

class NoFreeLocalMemoryPool {
 public:
  void Init(size_t chunk_size, size_t big_chunk_size = 0) {
    if (big_chunk_size == 0) {
      big_chunk_size = chunk_size;
    }
    big_chunk_size_ = big_chunk_size;
    chunk_size_ = chunk_size;
    CreateNewChunk();
  }

  ~NoFreeLocalMemoryPool() {
    Reset();
  }

  void Reset() {
    for (char* chunk: chunks_) {
      delete[] chunk;
    }
    chunks_.clear();

    for (char* chunk: big_chunks_) {
      delete[] chunk;
    }
    big_chunks_.clear();

    current_data_ = nullptr;
    free_size_ = 0;

    allocated_big_chunk_size_ = 0;
  }

  std::string MemoryInfo() const {
    std::string info;
    butil::string_printf(
        &info,
        "memory chunk:%lu,%lu, big:%lu,%lu",
        chunks_.size(),
        chunk_size_,
        big_chunks_.size(),
        allocated_big_chunk_size_
    );
    return info;
  }

  butil::StringPiece DataDup(const std::string& data) {
    size_t size = data.size();
    char* result = Malloc(size);
    memcpy(result, data.data(), data.size());
    return butil::StringPiece(result, data.size());
  }

  char* Malloc(size_t size) {
    char* result = nullptr;
    if (size < big_chunk_size_) {
      if (free_size_ < size) {
        CreateNewChunk();
      }
      result = current_data_;
      current_data_ += size;
      free_size_ -= size;
    } else {
      result = new char[size];
      big_chunks_.emplace_back(result);
      allocated_big_chunk_size_ += size;
    }
    return result;
  }

  static thread_local NoFreeLocalMemoryPool* tls;

 private:
  void CreateNewChunk() {
    char* chunk = new char[chunk_size_];
    chunks_.push_back(chunk);
    free_size_ = chunk_size_;
    current_data_ = chunk;
  }

  char* current_data_ = nullptr;
  size_t free_size_ = 0;

  size_t big_chunk_size_;
  size_t chunk_size_;
  std::vector<char*> chunks_;
  std::vector<char*> big_chunks_;
  size_t allocated_big_chunk_size_ = 0;
};

#if __cplusplus >= 202002L
template <typename T>
concept Allocator = requires(void* data, size_t size) {
  { T::Allocate(size) } -> std::same_as<void*>;
  { T::Deallocate(data, size) } -> std::same_as<void>;
};
#endif

class MallocAllocator {
 public:
  static void* Allocate(size_t size) {
    void* buf = malloc(size);
    // LOG(INFO) << "allocate buf: " << buf;
    return buf;
  }

  static void Deallocate(void* data, size_t size) {
    // LOG(INFO) << "deallocate buf: " << data ;
    (void) size;
    free(data);
  }
};

} // namespace index_engine
