#pragma once
#include <array>
#include <cstdint>

#if !defined(KVQUERY_FORCE_PORTABLE_SIMD) && defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#elif !defined(KVQUERY_FORCE_PORTABLE_SIMD) && defined(__SSE2__)
#include <emmintrin.h>
#if defined(__SSE4_1__)
#include <smmintrin.h>
#endif
#endif

namespace kvquery::detail {
inline const char* simd_backend_name() noexcept {
#if !defined(KVQUERY_FORCE_PORTABLE_SIMD) && defined(__aarch64__) && defined(__ARM_NEON)
  return "neon";
#elif !defined(KVQUERY_FORCE_PORTABLE_SIMD) && defined(__SSE2__)
#if defined(__SSE4_1__)
  return "sse4.1";
#else
  return "sse2";
#endif
#else
  return "portable";
#endif
}

// Bit i is set iff the full 64-bit keys in lane i are equal.
// Scalar arguments allow register construction without temporary array stores.
inline unsigned equal_key_mask(uint64_t w0, uint64_t w1,
                               uint64_t c0, uint64_t c1) noexcept {
#if !defined(KVQUERY_FORCE_PORTABLE_SIMD) && defined(__aarch64__) && defined(__ARM_NEON)
  const auto equal = vceqq_u64(vcombine_u64(vcreate_u64(w0), vcreate_u64(w1)),
                                vcombine_u64(vcreate_u64(c0), vcreate_u64(c1)));
  return static_cast<unsigned>(vgetq_lane_u64(equal, 0) >> 63) |
         (static_cast<unsigned>(vgetq_lane_u64(equal, 1) >> 63) << 1);
#elif !defined(KVQUERY_FORCE_PORTABLE_SIMD) && defined(__SSE2__)
  const auto a = _mm_set_epi64x(static_cast<long long>(w1), static_cast<long long>(w0));
  const auto b = _mm_set_epi64x(static_cast<long long>(c1), static_cast<long long>(c0));
#if defined(__SSE4_1__)
  return static_cast<unsigned>(_mm_movemask_pd(_mm_castsi128_pd(_mm_cmpeq_epi64(a, b))));
#else
  // SSE2 has no packed 64-bit integer equality: both 32-bit halves must match.
  const auto bytes = static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi32(a, b)));
  return static_cast<unsigned>((bytes & 0xffU) == 0xffU) |
         (static_cast<unsigned>((bytes & 0xff00U) == 0xff00U) << 1);
#endif
#else
  return static_cast<unsigned>(w0 == c0) |
         (static_cast<unsigned>(w1 == c1) << 1);
#endif
}
inline unsigned equal_key_mask(const std::array<uint64_t, 2>& wanted,
                               const std::array<uint64_t, 2>& candidate) noexcept {
  return equal_key_mask(wanted[0], wanted[1], candidate[0], candidate[1]);
}
}  // namespace kvquery::detail
