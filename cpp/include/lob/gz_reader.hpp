#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <vector>

struct gzFile_s;  // zlib's handle type; keeps <zlib.h> out of this header

namespace lob {

// Streams a gzipped ITCH 5.0 file and hands each framed message to a callback.
//
// On disk every message is a 2-byte big-endian length followed by that many bytes,
// the first of which is the message type. The reader decompresses into a fixed buffer;
// a message that straddles the end of the buffer is moved to the front and completed
// by the next read, so the callback always sees a contiguous message.
class GzItchReader {
 public:
  explicit GzItchReader(const std::filesystem::path& path, std::size_t buffer_bytes = 16u << 20);
  ~GzItchReader();
  GzItchReader(const GzItchReader&) = delete;
  GzItchReader& operator=(const GzItchReader&) = delete;

  // Calls on_message(const std::uint8_t* msg, std::uint16_t len) for every message, where
  // msg[0] is the type byte. Returns the number of messages. Throws if the file ends
  // partway through a message.
  template <class F>
  std::uint64_t for_each(F&& on_message) {
    std::uint64_t count = 0;
    while (fill()) {
      const std::uint8_t* b = buf_.data();
      while (end_ - pos_ >= 2) {
        const std::uint16_t len = static_cast<std::uint16_t>((b[pos_] << 8) | b[pos_ + 1]);
        if (end_ - pos_ - 2 < len) break;
        on_message(b + pos_ + 2, len);
        pos_ += 2 + static_cast<std::size_t>(len);
        ++count;
      }
    }
    if (pos_ != end_) throw std::runtime_error("ITCH file ends partway through a message");
    return count;
  }

 private:
  // Moves unconsumed bytes to the front and reads more. Returns false at end of file.
  bool fill();

  gzFile_s* file_ = nullptr;
  std::vector<std::uint8_t> buf_;
  std::size_t pos_ = 0;  // next unread byte
  std::size_t end_ = 0;  // one past the last valid byte
};

}  // namespace lob
