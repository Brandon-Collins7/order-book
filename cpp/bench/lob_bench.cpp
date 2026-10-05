// Benchmarks message parsing and order-book variants on a real ITCH file.
//
//   lob_bench <file.gz> --variant NAME [--symbols A,B,...] [--repeat N] [--sample-every K]
//             [--batch-mb MB] [--cpu C]
//
// Variants: frame, parse, baseline, pool/{std-hash,flat-hash,direct}/{map,vector}
//
// Method:
// - The file is decompressed into memory before timing starts. Files larger than one batch
//   are processed batch by batch; only the processing of each batch is timed.
// - Throughput: total messages / total processing time (wall clock, steady_clock).
// - Latency: every K-th message is timed on its own with the CPU timestamp counter (fenced
//   rdtsc / rdtscp). The timer's own overhead (the minimum of many empty measurements) is
//   subtracted. "Latency" is everything the engine does per message: decode, apply to the
//   book, and check whether the top of book changed.
// - Every variant reports a checksum over its whole top-of-book stream. All book variants must
//   report the same checksum as the baseline; otherwise one of them is wrong.
// - The process pins itself to one CPU (default 4, a performance core on Intel hybrid chips)
//   and raises its priority, to reduce scheduler noise.
//
// Output: one JSON object on stdout.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <intrin.h>
#else
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#include <x86intrin.h>
#endif

// With -DLOB_CACHEGRIND (CMake option LOB_CACHEGRIND), cachegrind only counts inside the timed
// replay, so the simulated cache misses exclude decompression and setup. Run it with
//   valgrind --tool=cachegrind --cache-sim=yes --instr-at-start=no ./lob_bench ...
#if defined(LOB_CACHEGRIND)
#include <valgrind/cachegrind.h>
#define LOB_COUNT_START CACHEGRIND_START_INSTRUMENTATION
#define LOB_COUNT_STOP CACHEGRIND_STOP_INSTRUMENTATION
#else
#define LOB_COUNT_START \
  do {              \
  } while (0)
#define LOB_COUNT_STOP \
  do {             \
  } while (0)
#endif

#include "lob/book.hpp"
#include "lob/endian.hpp"
#include "lob/fast_book.hpp"
#include "lob/gz_reader.hpp"
#include "lob/itch.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// ---- platform helpers -----------------------------------------------------------

inline std::uint64_t tsc_start() {
  _mm_lfence();
  return __rdtsc();
}
inline std::uint64_t tsc_stop() {
  unsigned aux;
  const std::uint64_t t = __rdtscp(&aux);
  _mm_lfence();
  return t;
}

void pin_and_prioritize(int cpu) {
  if (cpu < 0) return;
#if defined(_WIN32)
  SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1) << cpu);
  SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#else
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  sched_setaffinity(0, sizeof set, &set);
#endif
}

// Current resident memory of this process, in bytes.
std::uint64_t rss_now() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pmc{};
  GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
  return pmc.WorkingSetSize;
#else
  long pages = 0, resident = 0;
  if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
    if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
    std::fclose(f);
  }
  return static_cast<std::uint64_t>(resident) * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
#endif
}

std::string compiler() {
  std::ostringstream s;
#if defined(_MSC_VER) && !defined(__clang__)
  s << "MSVC " << _MSC_FULL_VER;
#elif defined(__clang__)
  s << "clang " << __clang_major__ << "." << __clang_minor__;
#elif defined(__GNUC__)
  s << "gcc " << __GNUC__ << "." << __GNUC_MINOR__;
#endif
  return s.str();
}

// ---- workloads --------------------------------------------------------------------

constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;
inline void mix(std::uint64_t& h, std::uint64_t v) { h = (h ^ v) * kFnvPrime; }

// Walks the framing only: the floor for any per-message work.
struct FrameOnly {
  static std::string name() { return "frame"; }
  std::uint64_t acc = 0;
  void operator()(const std::uint8_t* m, std::uint16_t len) { acc += m[0] + len; }
  std::uint64_t checksum() const { return acc; }
};

// Decodes every message and touches its fields, but keeps no book.
struct ParseOnly : lob::itch::Handler {
  static std::string name() { return "parse"; }
  std::uint64_t acc = 0;
  template <class M>
  void on(const M& m) { acc += m.timestamp + m.locate; }
  void on(const lob::itch::AddOrder& m) { acc += m.ref + m.shares + m.price + static_cast<std::uint8_t>(m.side); }
  void on(const lob::itch::OrderExecuted& m) { acc += m.ref + m.shares; }
  void on(const lob::itch::OrderCancel& m) { acc += m.ref + m.shares; }
  void on(const lob::itch::OrderDelete& m) { acc += m.ref; }
  void on(const lob::itch::OrderReplace& m) { acc += m.orig_ref + m.new_ref + m.shares + m.price; }
  void operator()(const std::uint8_t* m, std::uint16_t len) { lob::itch::dispatch(m, len, *this); }
  std::uint64_t checksum() const { return acc; }
};

// Applies each message to a book builder and tracks top-of-book changes, as build_book does.
template <class Builder>
struct BookWork {
  static std::string name() {
    if constexpr (requires { Builder::name(); }) return Builder::name();
    else return "baseline";
  }
  explicit BookWork(const std::vector<std::string>& symbols) : builder(symbols), last(65536) {}

  void operator()(const std::uint8_t* m, std::uint16_t len) {
    builder.process(m, len);
    if (const auto* b = builder.touched()) {
      const lob::Top t = b->top();
      lob::Top& prev = last[builder.touched_locate()];
      if (!(t == prev)) {
        prev = t;
        ++changes;
        mix(hash, lob::be48(m + 5));
        mix(hash, builder.touched_locate());
        mix(hash, t.bid_px);
        mix(hash, t.bid_sz);
        mix(hash, t.ask_px);
        mix(hash, t.ask_sz);
      }
    }
  }
  std::uint64_t checksum() const { return hash; }

  Builder builder;
  std::vector<lob::Top> last;
  std::uint64_t changes = 0;
  std::uint64_t hash = kFnvOffset;
};

// ---- harness --------------------------------------------------------------------------

struct Options {
  std::string file;
  std::string variant;
  std::vector<std::string> symbols;
  int repeat = 5;
  std::uint32_t sample_every = 64;
  std::size_t batch_bytes = 512u << 20;
  int cpu = 4;
};

struct Timer {
  double ticks_per_ns = 0;
  std::uint64_t overhead_ticks = 0;

  void calibrate() {
    const auto c0 = Clock::now();
    const std::uint64_t t0 = tsc_start();
    while (Clock::now() - c0 < std::chrono::milliseconds(200)) {
    }
    const std::uint64_t t1 = tsc_stop();
    const double ns = std::chrono::duration<double, std::nano>(Clock::now() - c0).count();
    ticks_per_ns = static_cast<double>(t1 - t0) / ns;
    overhead_ticks = ~0ull;
    for (int i = 0; i < 100'000; ++i) {
      const std::uint64_t a = tsc_start();
      const std::uint64_t b = tsc_stop();
      overhead_ticks = std::min(overhead_ticks, b - a);
    }
  }
  double to_ns(std::uint64_t ticks) const {
    return ticks > overhead_ticks ? static_cast<double>(ticks - overhead_ticks) / ticks_per_ns : 0.0;
  }
};

struct RunResult {
  double seconds = 0;
  std::uint64_t messages = 0;
  std::uint64_t checksum = 0;
  std::uint64_t top_changes = 0;
  std::vector<std::uint64_t> samples;
  std::uint64_t mem_bytes = 0;
  lob::BookStats stats;
  std::uint64_t live_orders = 0;
};

// Replays one in-memory batch of framed messages, timing every sample_every-th message alone.
// Also samples resident memory every 2^20 messages into max_rss (a few microseconds each).
template <class Work>
std::uint64_t replay(const std::vector<std::uint8_t>& batch, Work& work, std::uint32_t sample_every,
                     std::uint32_t& countdown, std::vector<std::uint64_t>& samples, std::uint64_t& max_rss) {
  const std::uint8_t* p = batch.data();
  const std::uint8_t* const end = p + batch.size();
  std::uint64_t n = 0;
  while (p < end) {
    if ((n & ((1u << 20) - 1)) == 0) max_rss = std::max(max_rss, rss_now());
    const std::uint16_t len = lob::be16(p);
    if (--countdown == 0) {
      countdown = sample_every;
      const std::uint64_t t0 = tsc_start();
      work(p + 2, len);
      const std::uint64_t t1 = tsc_stop();
      samples.push_back(t1 - t0);
    } else {
      work(p + 2, len);
    }
    p += 2 + static_cast<std::size_t>(len);
    ++n;
  }
  return n;
}

// The data source: either the whole file in memory (repeatable), or batches streamed from disk.
class Source {
 public:
  explicit Source(const Options& o) : opt_(o) {
    lob::GzItchReader reader(opt_.file);
    if (!reader.next_batch(buf_, opt_.batch_bytes)) throw std::runtime_error("empty file");
    std::vector<std::uint8_t> probe;
    in_memory_ = !reader.next_batch(probe, 1);  // nothing left: the whole file fits in one batch
  }

  bool in_memory() const { return in_memory_; }

  // Calls f(batch) for each batch of the file, in order. The buffer is reused, so memory
  // use stays at one batch and is already allocated before any timed run starts.
  template <class F>
  void for_each_batch(F&& f) {
    if (in_memory_) {
      f(buf_);
      return;
    }
    lob::GzItchReader reader(opt_.file);
    while (reader.next_batch(buf_, opt_.batch_bytes)) f(buf_);
  }

 private:
  const Options& opt_;
  std::vector<std::uint8_t> buf_;
  bool in_memory_ = false;
};

template <class Work, class... Args>
RunResult run_once(Source& src, const Options& opt, Args&&... args) {
  RunResult r;
  // Memory growth = highest resident memory sampled during the run minus resident memory just
  // before it. (The process-wide peak is no use here: loading the file sets it before timing.)
  const std::uint64_t mem_before = rss_now();
  std::uint64_t max_rss = mem_before;
  Work work(std::forward<Args>(args)...);
  std::uint32_t countdown = opt.sample_every;
  src.for_each_batch([&](const std::vector<std::uint8_t>& batch) {
    const auto t0 = Clock::now();
    LOB_COUNT_START;
    r.messages += replay(batch, work, opt.sample_every, countdown, r.samples, max_rss);
    LOB_COUNT_STOP;
    r.seconds += std::chrono::duration<double>(Clock::now() - t0).count();
  });
  max_rss = std::max(max_rss, rss_now());
  r.mem_bytes = max_rss - mem_before;
  r.checksum = work.checksum();
  if constexpr (requires { work.builder; }) {
    r.top_changes = work.changes;
    r.stats = work.builder.stats();
    r.live_orders = work.builder.live_orders();
  }
  return r;
}

template <class Work, class... Args>
void bench(Source& src, const Options& opt, const Timer& timer, Args&&... args) {
  const int repeat = src.in_memory() ? opt.repeat : 1;
  std::vector<RunResult> runs;
  for (int i = 0; i < repeat; ++i) runs.push_back(run_once<Work>(src, opt, args...));

  std::vector<double> secs;
  for (const auto& r : runs) secs.push_back(r.seconds);
  std::vector<double> sorted_secs = secs;
  std::sort(sorted_secs.begin(), sorted_secs.end());
  const double median = sorted_secs[sorted_secs.size() / 2];

  // Latency percentiles come from the last run, when caches and allocators are warm.
  std::vector<std::uint64_t> s = runs.back().samples;
  std::sort(s.begin(), s.end());
  auto pct = [&](double q) {
    if (s.empty()) return 0.0;
    return timer.to_ns(s[std::min(s.size() - 1, static_cast<std::size_t>(q * static_cast<double>(s.size())))]);
  };

  const RunResult& r = runs.front();  // memory is only meaningful for the first run
  for (const auto& x : runs)
    if (x.checksum != r.checksum) throw std::runtime_error("checksum differs between repeats");

  std::printf("{\"variant\": \"%s\", \"file\": \"%s\", \"symbols\": \"", Work::name().c_str(), opt.file.c_str());
  for (std::size_t i = 0; i < opt.symbols.size(); ++i) std::printf("%s%s", i ? "," : "", opt.symbols[i].c_str());
  std::printf("\", \"messages\": %llu, \"runs_seconds\": [", static_cast<unsigned long long>(r.messages));
  for (std::size_t i = 0; i < secs.size(); ++i) std::printf("%s%.4f", i ? ", " : "", secs[i]);
  std::printf("], \"median_seconds\": %.4f, \"msgs_per_sec\": %.0f, \"ns_per_msg\": %.2f,", median,
              static_cast<double>(r.messages) / median, median * 1e9 / static_cast<double>(r.messages));
  std::printf(" \"latency_ns\": {\"p50\": %.1f, \"p90\": %.1f, \"p99\": %.1f, \"p99.9\": %.1f, \"max\": %.1f},",
              pct(0.50), pct(0.90), pct(0.99), pct(0.999), s.empty() ? 0.0 : timer.to_ns(s.back()));
  std::printf(" \"latency_samples\": %zu, \"sample_every\": %u, \"timer_overhead_ns\": %.1f,", s.size(),
              opt.sample_every, static_cast<double>(timer.overhead_ticks) / timer.ticks_per_ns);
  std::printf(" \"mem_growth_mb\": %.1f, \"top_changes\": %llu, \"checksum\": \"%016llx\",",
              static_cast<double>(r.mem_bytes) / 1e6, static_cast<unsigned long long>(r.top_changes),
              static_cast<unsigned long long>(r.checksum));
  std::printf(" \"peak_live_orders\": %llu, \"live_orders_end\": %llu,",
              static_cast<unsigned long long>(r.stats.max_live_orders), static_cast<unsigned long long>(r.live_orders));
  std::printf(" \"integrity\": {\"unknown_ref\": %llu, \"duplicate_ref\": %llu, \"overfill\": %llu},",
              static_cast<unsigned long long>(r.stats.unknown_ref),
              static_cast<unsigned long long>(r.stats.duplicate_ref), static_cast<unsigned long long>(r.stats.overfill));
  std::printf(" \"in_memory\": %s, \"cpu\": %d, \"compiler\": \"%s\"}\n", src.in_memory() ? "true" : "false", opt.cpu,
              compiler().c_str());
}

template <class Index, template <bool> class Levels>
void bench_pool(Source& src, const Options& opt, const Timer& t) {
  bench<BookWork<lob::fast::BookBuilder<Index, Levels>>>(src, opt, t, opt.symbols);
}

const std::map<std::string, std::function<void(Source&, const Options&, const Timer&)>>& variants() {
  using namespace lob::fast;
  static const std::map<std::string, std::function<void(Source&, const Options&, const Timer&)>> v = {
      {"frame", [](Source& s, const Options& o, const Timer& t) { bench<FrameOnly>(s, o, t); }},
      {"parse", [](Source& s, const Options& o, const Timer& t) { bench<ParseOnly>(s, o, t); }},
      {"baseline",
       [](Source& s, const Options& o, const Timer& t) { bench<BookWork<lob::BookBuilder>>(s, o, t, o.symbols); }},
      {"pool/std-hash/map", bench_pool<StdHashIndex, MapLevels>},
      {"pool/std-hash/vector", bench_pool<StdHashIndex, VectorLevels>},
      {"pool/flat-hash/map", bench_pool<FlatHashIndex, MapLevels>},
      {"pool/flat-hash/vector", bench_pool<FlatHashIndex, VectorLevels>},
      {"pool/direct/map", bench_pool<DirectIndex, MapLevels>},
      {"pool/direct/vector", bench_pool<DirectIndex, VectorLevels>},
  };
  return v;
}

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  for (std::string item; std::getline(ss, item, ',');)
    if (!item.empty()) out.push_back(item);
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string a = argv[i];
      auto value = [&]() -> std::string {
        if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
        return argv[++i];
      };
      if (a == "--variant") opt.variant = value();
      else if (a == "--symbols") opt.symbols = split(value());
      else if (a == "--repeat") opt.repeat = std::stoi(value());
      else if (a == "--sample-every") opt.sample_every = static_cast<std::uint32_t>(std::stoul(value()));
      else if (a == "--batch-mb") opt.batch_bytes = std::stoull(value()) << 20;
      else if (a == "--cpu") opt.cpu = std::stoi(value());
      else if (opt.file.empty()) opt.file = a;
      else throw std::runtime_error("unexpected argument " + a);
    }
    const auto it = variants().find(opt.variant);
    if (opt.file.empty() || it == variants().end()) {
      std::fprintf(stderr, "usage: %s <file.gz> --variant NAME [--symbols A,B] [--repeat N] [--sample-every K] "
                   "[--batch-mb MB] [--cpu C]\nvariants:", argv[0]);
      for (const auto& [name, f] : variants()) std::fprintf(stderr, " %s", name.c_str());
      std::fprintf(stderr, "\n");
      return 2;
    }

    pin_and_prioritize(opt.cpu);
    Timer timer;
    timer.calibrate();
    Source src(opt);
    it->second(src, opt, timer);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return 0;
}
