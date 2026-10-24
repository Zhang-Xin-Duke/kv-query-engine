#pragma once
#include <array>
#include <cstdint>
#include <cstring>

#if !defined(KVQUERY_FORCE_PORTABLE_SIMD) && defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#elif !defined(KVQUERY_FORCE_PORTABLE_SIMD) && defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace kvquery::detail {
inline const char* simd_backend_name() noexcept {
#if !defined(KVQUERY_FORCE_PORTABLE_SIMD) && defined(__aarch64__) && defined(__ARM_NEON)
  return "neon";
#elif !defined(KVQUERY_FORCE_PORTABLE_SIMD) && defined(__SSE2__)
  return "sse2";
#else
  return "portable";
#endif
}

// Bit i is set iff the full 64-bit keys in lane i are equal.
// Inputs are fully initialized local arrays, including inactive tail lanes.
inline unsigned equal_key_mask(const std::array<uint64_t, 2>& wanted,
                               const std::array<uint64_t, 2>& candidate) noexcept {
#if !defined(KVQUERY_FORCE_PORTABLE_SIMD) && defined(__aarch64__) && defined(__ARM_NEON)
  const auto equal = vceqq_u64(vld1q_u64(wanted.data()), vld1q_u64(candidate.data()));
  return static_cast<unsigned>(vgetq_lane_u64(equal, 0) >> 63) |
         (static_cast<unsigned>(vgetq_lane_u64(equal, 1) >> 63) << 1);
#elif !defined(KVQUERY_FORCE_PORTABLE_SIMD) && defined(__SSE2__)
  __m128i a, b;
  std::memcpy(&a, wanted.data(), sizeof(a));
  std::memcpy(&b, candidate.data(), sizeof(b));
  // SSE2 has no packed 64-bit integer equality: both 32-bit halves must match.
  const auto bytes = static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi32(a, b)));
  return static_cast<unsigned>((bytes & 0xffU) == 0xffU) |
         (static_cast<unsigned>((bytes & 0xff00U) == 0xff00U) << 1);
#else
  return static_cast<unsigned>(wanted[0] == candidate[0]) |
         (static_cast<unsigned>(wanted[1] == candidate[1]) << 1);
#endif
}
}  // namespace kvquery::detail
