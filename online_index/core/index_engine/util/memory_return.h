#pragma once

#include <stdexcept>
#include <cstddef>
#include <string_view>

#ifndef ENGINE_HAVE_MALLOC_TRIM
#define ENGINE_HAVE_MALLOC_TRIM 0
#endif
#if ENGINE_HAVE_MALLOC_TRIM
#include <malloc.h>
#endif

namespace index_engine {

enum class MemoryReturnPolicy { None, Trim, Reclaim };

inline constexpr bool MemoryReturnAvailable() {
  return ENGINE_HAVE_MALLOC_TRIM != 0;
}

inline void ValidateMemoryReturnPolicy(MemoryReturnPolicy policy) {
  if (policy != MemoryReturnPolicy::None && !MemoryReturnAvailable())
    throw std::invalid_argument("trim memory return requires a glibc malloc build");
}

inline MemoryReturnPolicy ParseMemoryReturnPolicy(std::string_view text) {
  if (text == "none") return MemoryReturnPolicy::None;
  auto policy = text == "trim" ? MemoryReturnPolicy::Trim : MemoryReturnPolicy::Reclaim;
  if (text != "trim" && text != "reclaim")
    throw std::invalid_argument("memory return must be none, trim or reclaim");
  ValidateMemoryReturnPolicy(policy);
  return policy;
}

// Best effort: zero from malloc_trim means no pages were returned, not failure.
// This can affect unused pages in every glibc arena, not just this table.
inline void ReturnUnusedMemory(MemoryReturnPolicy policy) {
#if ENGINE_HAVE_MALLOC_TRIM
  if (policy != MemoryReturnPolicy::None) (void)malloc_trim(0);
#else
  ValidateMemoryReturnPolicy(policy);
#endif
}

// One writer configures and advances this schedule; readers never inspect it.
class MemoryReturnSchedule {
 public:
  void Configure(MemoryReturnPolicy policy, size_t every = 1) {
    ValidateMemoryReturnPolicy(policy);
    if (every == 0) throw std::invalid_argument("trim-every must be positive");
    if (every != 1 && policy != MemoryReturnPolicy::Reclaim)
      throw std::invalid_argument("trim-every requires reclaim memory return");
    policy_ = policy;
    every_ = every;
    pending_ = 0;
  }

  bool ReplacementReady() const { return policy_ == MemoryReturnPolicy::Trim; }

  bool Reclaimed(bool last) {
    if (policy_ == MemoryReturnPolicy::Trim) return true;
    if (policy_ != MemoryReturnPolicy::Reclaim) return false;
    ++pending_;
    if (pending_ < every_ && !last) return false;
    pending_ = 0;
    return true;
  }

 private:
  MemoryReturnPolicy policy_{MemoryReturnPolicy::None};
  size_t every_{1};
  size_t pending_{0};
};

}  // namespace index_engine
