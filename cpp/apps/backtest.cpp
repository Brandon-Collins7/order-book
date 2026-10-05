// Runs the market maker over a gzipped ITCH file and writes every fill with its markouts.
//
//   backtest <file.gz> --symbols AAPL,MSFT [--out fills.csv] [--latency-us 10]
//            [--cancel-model proportional|pessimistic] [--size 100] [--max-pos 500]
//            [--k 0] [--theta 2] [--levels 1] [--cooldown-ms 1000] [--stop-before-close-s 60]
//
// stdout: one JSON summary (config, per-symbol orders/fills/position/PnL).
// --out:  CSV with one row per fill; prices in dollars; mid_* are the mid prices at each
//         markout horizon after the fill (0 if the book had an empty side).

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "lob/gz_reader.hpp"
#include "lob/sim.hpp"
#include "lob/strategy.hpp"

namespace {

// Escapes a string for a JSON string literal (Windows paths contain backslashes).
std::string json_escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  return out;
}

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  for (std::string item; std::getline(ss, item, ',');)
    if (!item.empty()) out.push_back(item);
  return out;
}

void write_fills(const std::string& path, const lob::sim::Simulator& sim,
                 const std::vector<lob::sim::Simulator::SymbolResult>& results) {
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (!f) throw std::runtime_error("cannot write " + path);
  std::vector<std::string> names(65536);
  for (const auto& r : results) names[r.locate] = r.symbol;
  std::fprintf(f, "ts_ns,symbol,side,price,shares,position,mid,reason,ahead_at_arrival,order_age_ns,"
                  "mid_100ms,mid_1s,mid_5s,mid_30s,imbalance,spread\n");
  const double scale = lob::itch::kPriceScale;
  for (const auto& x : sim.fills()) {
    std::fprintf(f, "%llu,%s,%c,%.4f,%u,%lld,%.5f,%c,%llu,%llu,%.5f,%.5f,%.5f,%.5f,%.4f,%.4f\n",
                 static_cast<unsigned long long>(x.ts), names[x.locate].c_str(), x.side, x.price / scale, x.shares,
                 static_cast<long long>(x.position_after), x.mid / scale, static_cast<char>(x.reason),
                 static_cast<unsigned long long>(x.ahead_at_arrival), static_cast<unsigned long long>(x.order_age_ns),
                 x.mid_after[0] / scale, x.mid_after[1] / scale, x.mid_after[2] / scale, x.mid_after[3] / scale,
                 x.imbalance, x.spread / scale);
  }
  std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::string file, out;
    std::vector<std::string> symbols;
    lob::sim::Config cfg;
    lob::sim::MarketMakerParams mm;
    for (int i = 1; i < argc; ++i) {
      const std::string a = argv[i];
      auto value = [&]() -> std::string {
        if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
        return argv[++i];
      };
      if (a == "--symbols") symbols = split(value());
      else if (a == "--out") out = value();
      else if (a == "--latency-us") cfg.latency_ns = static_cast<std::uint64_t>(std::stod(value()) * 1000);
      else if (a == "--cancel-model") {
        const std::string m = value();
        if (m == "proportional") cfg.cancel_model = lob::sim::CancelModel::kProportional;
        else if (m == "pessimistic") cfg.cancel_model = lob::sim::CancelModel::kPessimistic;
        else throw std::runtime_error("unknown cancel model " + m);
      } else if (a == "--size") mm.size = static_cast<std::uint32_t>(std::stoul(value()));
      else if (a == "--max-pos") mm.max_position = std::stoll(value());
      else if (a == "--k") mm.skew_k = std::stod(value());
      else if (a == "--theta") mm.imbalance_theta = std::stod(value());
      else if (a == "--levels") mm.imbalance_levels = std::stoul(value());
      else if (a == "--cooldown-ms") cfg.reopen_cooldown_ns = static_cast<std::uint64_t>(std::stod(value()) * 1e6);
      else if (a == "--stop-before-close-s")
        cfg.stop_before_close_ns = static_cast<std::uint64_t>(std::stod(value()) * 1e9);
      else if (file.empty()) file = a;
      else throw std::runtime_error("unexpected argument " + a);
    }
    if (file.empty() || symbols.empty()) {
      std::fprintf(stderr,
                   "usage: %s <file.gz> --symbols A,B [--out fills.csv] [--latency-us 10] "
                   "[--cancel-model proportional|pessimistic] [--size 100] [--max-pos 500] [--k 0] [--theta 2] "
                   "[--levels 1] [--cooldown-ms 1000] [--stop-before-close-s 60]\n",
                   argv[0]);
      return 2;
    }

    lob::sim::MarketMaker strategy(mm);
    lob::sim::Simulator sim(symbols, strategy, cfg);
    const auto t0 = std::chrono::steady_clock::now();
    lob::GzItchReader reader(file);
    const std::uint64_t messages =
        reader.for_each([&](const std::uint8_t* m, std::uint16_t len) { sim.process(m, len); });
    sim.finish();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const auto results = sim.results();
    if (!out.empty()) write_fills(out, sim, results);

    std::printf("{\"file\": \"%s\", \"strategy\": \"%s\", \"latency_us\": %.3f, \"cancel_model\": \"%s\", "
                "\"messages\": %llu, \"seconds\": %.2f, \"fills\": %zu, \"symbols\": [",
                json_escape(file).c_str(), strategy.name().c_str(), static_cast<double>(cfg.latency_ns) / 1000.0,
                cfg.cancel_model == lob::sim::CancelModel::kProportional ? "proportional" : "pessimistic",
                static_cast<unsigned long long>(messages), secs, sim.fills().size());
    for (std::size_t i = 0; i < results.size(); ++i) {
      const auto& r = results[i];
      std::printf("%s{\"symbol\": \"%s\", \"orders_sent\": %llu, \"cancels_sent\": %llu, \"rejects\": %llu, "
                  "\"fills\": %llu, \"bought\": %llu, \"sold\": %llu, \"final_position\": %lld, \"cash\": %.2f, "
                  "\"close_mid\": %.4f, \"pnl\": %.2f}",
                  i ? ", " : "", r.symbol.c_str(), static_cast<unsigned long long>(r.stats.orders_sent),
                  static_cast<unsigned long long>(r.stats.cancels_sent), static_cast<unsigned long long>(r.stats.rejects),
                  static_cast<unsigned long long>(r.stats.fills), static_cast<unsigned long long>(r.stats.bought),
                  static_cast<unsigned long long>(r.stats.sold), static_cast<long long>(r.position), r.cash,
                  r.last_mid, r.pnl);
    }
    std::printf("]}\n");
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return 0;
}
