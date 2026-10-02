#pragma once

// Big-endian loads for ITCH fields. ITCH is big-endian on the wire; x86 and ARM hosts are
// little-endian, so each load is a memcpy (safe for unaligned data) plus a byte swap. Both
// compile down to a single mov + bswap (or movbe).

#include <bit>
#include <cstdint>
#include <cstring>

#if defined(_MSC_VER)
#include <stdlib.h>
#endif

namespace lob {

static_assert(std::endian::native == std::endian::little, "big-endian hosts are not supported");

inline std::uint16_t bswap(std::uint16_t v) {
#if defined(_MSC_VER)
  return _byteswap_ushort(v);
#else
  return __builtin_bswap16(v);
#endif
}

inline std::uint32_t bswap(std::uint32_t v) {
#if defined(_MSC_VER)
  return _byteswap_ulong(v);
#else
  return __builtin_bswap32(v);
#endif
}

inline std::uint64_t bswap(std::uint64_t v) {
#if defined(_MSC_VER)
  return _byteswap_uint64(v);
#else
  return __builtin_bswap64(v);
#endif
}

template <class T>
inline T load_be(const std::uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof v);
  return bswap(v);
}

inline std::uint16_t be16(const std::uint8_t* p) { return load_be<std::uint16_t>(p); }
inline std::uint32_t be32(const std::uint8_t* p) { return load_be<std::uint32_t>(p); }
inline std::uint64_t be64(const std::uint8_t* p) { return load_be<std::uint64_t>(p); }

// ITCH timestamps are 6 bytes: nanoseconds since midnight.
inline std::uint64_t be48(const std::uint8_t* p) {
  return (static_cast<std::uint64_t>(be16(p)) << 32) | be32(p + 2);
}

}  // namespace lob
