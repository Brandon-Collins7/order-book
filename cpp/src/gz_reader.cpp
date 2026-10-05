#include "lob/gz_reader.hpp"

#include <zlib.h>

#include <cstring>
#include <string>

namespace lob {

GzItchReader::GzItchReader(const std::filesystem::path& path, std::size_t buffer_bytes)
    : buf_(buffer_bytes) {
  if (buffer_bytes < 2 + 65535) throw std::invalid_argument("buffer must hold the largest message");
  file_ = gzopen(path.string().c_str(), "rb");
  if (!file_) throw std::runtime_error("cannot open " + path.string());
  gzbuffer(file_, 1u << 20);
}

GzItchReader::~GzItchReader() {
  if (file_) gzclose(file_);
}

bool GzItchReader::fill() {
  const std::size_t left = end_ - pos_;
  if (left && pos_) std::memmove(buf_.data(), buf_.data() + pos_, left);
  pos_ = 0;
  end_ = left;
  const int got = gzread(file_, buf_.data() + end_, static_cast<unsigned>(buf_.size() - end_));
  if (got < 0) {
    int err = 0;
    throw std::runtime_error(std::string("gzread failed: ") + gzerror(file_, &err));
  }
  end_ += static_cast<std::size_t>(got);
  return got > 0;
}

bool GzItchReader::next_batch(std::vector<std::uint8_t>& out, std::size_t target_bytes) {
  out.clear();
  for (;;) {
    while (end_ - pos_ >= 2) {
      const std::size_t len = (static_cast<std::size_t>(buf_[pos_]) << 8) | buf_[pos_ + 1];
      if (end_ - pos_ - 2 < len) break;
      out.insert(out.end(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_),
                 buf_.begin() + static_cast<std::ptrdiff_t>(pos_ + 2 + len));
      pos_ += 2 + len;
      if (out.size() >= target_bytes) return true;
    }
    if (!fill()) {
      if (pos_ != end_) throw std::runtime_error("ITCH file ends partway through a message");
      return !out.empty();
    }
  }
}

}  // namespace lob
