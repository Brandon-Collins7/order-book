#pragma once

// Order book variants built for speed, to benchmark against the baseline in lob/book.hpp.
// Each design choice is a template parameter, so its effect can be measured on its own.
//
// What changes compared with the baseline:
// - Orders live in one contiguous pool (std::vector) and are recycled through a free list,
//   instead of one heap allocation per order. The FIFO links are 32-bit pool indices rather
//   than pointers, so the pool can grow without breaking them, and an Order is 32 bytes.
// - The order-ref -> pool-index lookup is one of three Index types:
//     StdHashIndex     std::unordered_map (node-based, one allocation per entry)
//     FlatHashIndex    open addressing with linear probing (one flat array)
//     DirectIndex      a plain array indexed by the ref itself (ITCH refs are dense)
// - Price levels per side are either
//     MapLevels        std::map, as in the baseline
//     VectorLevels     a sorted std::vector with the best price at the back, because almost
//                      all activity is at or near the best price

#include <algorithm>
#include <bit>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "lob/book.hpp"  // Top, BookStats
#include "lob/itch.hpp"

namespace lob::fast {

using itch::Price;
constexpr std::uint32_t kNil = 0xFFFFFFFFu;

struct Order {
  std::uint64_t ref;
  Price price;
  std::uint32_t shares;
  std::uint32_t prev;  // pool index of the order ahead in the queue, or kNil
  std::uint32_t next;  // pool index of the order behind, or kNil
  std::uint16_t locate;
  char side;
};
static_assert(sizeof(Order) == 32);

// Contiguous storage for orders. Freed slots are reused last-in-first-out, so a new order
// usually lands in memory that is still in cache.
class OrderPool {
 public:
  std::uint32_t alloc() {
    if (free_ != kNil) {
      const std::uint32_t i = free_;
      free_ = orders_[i].next;
      return i;
    }
    orders_.emplace_back();
    return static_cast<std::uint32_t>(orders_.size() - 1);
  }
  void release(std::uint32_t i) {
    orders_[i].next = free_;
    free_ = i;
  }
  Order& operator[](std::uint32_t i) { return orders_[i]; }
  const Order& operator[](std::uint32_t i) const { return orders_[i]; }
  std::size_t bytes() const { return orders_.capacity() * sizeof(Order); }

 private:
  std::vector<Order> orders_;
  std::uint32_t free_ = kNil;
};

// ---- ref -> pool index ---------------------------------------------------------

class StdHashIndex {
 public:
  static constexpr const char* kName = "std-hash";
  std::uint32_t find(std::uint64_t ref) const {
    auto it = map_.find(ref);
    return it == map_.end() ? kNil : it->second;
  }
  bool insert(std::uint64_t ref, std::uint32_t idx) { return map_.try_emplace(ref, idx).second; }
  void erase(std::uint64_t ref) { map_.erase(ref); }

 private:
  std::unordered_map<std::uint64_t, std::uint32_t> map_;
};

// Open addressing with linear probing, kept at most half full. Deletion shifts later entries
// back instead of leaving tombstones, which matters here because every order is eventually
// deleted: tombstones would pile up and slow every lookup.
class FlatHashIndex {
 public:
  static constexpr const char* kName = "flat-hash";

  FlatHashIndex() { rehash(1u << 16); }

  std::uint32_t find(std::uint64_t ref) const {
    for (std::size_t i = home(ref);; i = (i + 1) & mask_) {
      if (slots_[i].key == ref) return slots_[i].value;
      if (slots_[i].key == kEmpty) return kNil;
    }
  }

  bool insert(std::uint64_t ref, std::uint32_t idx) {
    if ((size_ + 1) * 2 > slots_.size()) rehash(slots_.size() * 2);
    for (std::size_t i = home(ref);; i = (i + 1) & mask_) {
      if (slots_[i].key == ref) return false;
      if (slots_[i].key == kEmpty) {
        slots_[i] = {ref, idx};
        ++size_;
        return true;
      }
    }
  }

  void erase(std::uint64_t ref) {
    std::size_t i = home(ref);
    for (;; i = (i + 1) & mask_) {
      if (slots_[i].key == ref) break;
      if (slots_[i].key == kEmpty) return;
    }
    // Backward-shift deletion: pull later entries of the probe run into the hole, unless an
    // entry's home slot lies in the cyclic range (i, j], in which case it must stay put.
    for (std::size_t j = i;;) {
      j = (j + 1) & mask_;
      if (slots_[j].key == kEmpty) break;
      const std::size_t h = home(slots_[j].key);
      const bool stays = i <= j ? (i < h && h <= j) : (i < h || h <= j);
      if (!stays) {
        slots_[i] = slots_[j];
        i = j;
      }
    }
    slots_[i].key = kEmpty;
    --size_;
  }

  std::size_t size() const { return size_; }

 private:
  struct Slot {
    std::uint64_t key;
    std::uint32_t value;
  };
  static constexpr std::uint64_t kEmpty = ~0ull;

  // Fibonacci hashing: multiply by 2^64 / golden ratio and keep the top bits.
  std::size_t home(std::uint64_t key) const {
    return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
  }

  void rehash(std::size_t capacity) {
    std::vector<Slot> old = std::move(slots_);
    slots_.assign(capacity, Slot{kEmpty, 0});
    mask_ = capacity - 1;
    shift_ = 64 - std::countr_zero(capacity);
    size_ = 0;
    for (const Slot& s : old)
      if (s.key != kEmpty) insert(s.key, s.value);
  }

  std::vector<Slot> slots_;
  std::size_t mask_ = 0;
  int shift_ = 0;
  std::size_t size_ = 0;
};

// One 4-byte slot per possible ref. ITCH refs count up from zero through the day, so this is
// the fastest lookup possible, at the cost of memory proportional to the day's highest ref,
// not to the number of live orders.
class DirectIndex {
 public:
  static constexpr const char* kName = "direct";
  std::uint32_t find(std::uint64_t ref) const { return ref < slots_.size() ? slots_[ref] : kNil; }
  bool insert(std::uint64_t ref, std::uint32_t idx) {
    if (ref >= slots_.size()) slots_.resize(std::max<std::size_t>(ref + 1, slots_.size() * 2), kNil);
    if (slots_[ref] != kNil) return false;
    slots_[ref] = idx;
    return true;
  }
  void erase(std::uint64_t ref) { slots_[ref] = kNil; }
  std::size_t bytes() const { return slots_.capacity() * sizeof(std::uint32_t); }

 private:
  std::vector<std::uint32_t> slots_;
};

// ---- price levels --------------------------------------------------------------

struct Level {
  Price price = 0;
  std::uint32_t orders = 0;
  std::uint64_t shares = 0;
  std::uint32_t head = kNil;  // oldest order
  std::uint32_t tail = kNil;
};

template <bool kBid>
class MapLevels {
 public:
  static constexpr const char* kName = "map";
  Level& get_or_add(Price p) {
    Level& lv = map_[p];
    lv.price = p;
    return lv;
  }
  Level& at(Price p) { return map_.find(p)->second; }
  void erase(Price p) { map_.erase(p); }
  const Level* best() const { return map_.empty() ? nullptr : &map_.begin()->second; }

 private:
  using Better = std::conditional_t<kBid, std::greater<Price>, std::less<Price>>;
  std::map<Price, Level, Better> map_;
};

template <bool kBid>
class VectorLevels {
 public:
  static constexpr const char* kName = "vector";
  Level& get_or_add(Price p) {
    const std::size_t i = position(p);
    if (i < levels_.size() && levels_[i].price == p) return levels_[i];
    Level lv;
    lv.price = p;
    return *levels_.insert(levels_.begin() + static_cast<std::ptrdiff_t>(i), lv);
  }
  Level& at(Price p) { return levels_[position(p)]; }
  void erase(Price p) { levels_.erase(levels_.begin() + static_cast<std::ptrdiff_t>(position(p))); }
  const Level* best() const { return levels_.empty() ? nullptr : &levels_.back(); }

 private:
  static bool worse(Price a, Price b) { return kBid ? a < b : a > b; }

  // Index of the first level that is not worse than p (where p is, or would be inserted).
  // Levels run from worst to best, so scan back from the best price first: almost every
  // update lands within a few levels of it. Fall back to binary search for deep prices.
  std::size_t position(Price p) const {
    std::size_t i = levels_.size();
    const std::size_t stop = i > 8 ? i - 8 : 0;
    while (i > stop && !worse(levels_[i - 1].price, p)) --i;
    if (i == stop && stop > 0) {
      auto it = std::lower_bound(levels_.begin(), levels_.begin() + static_cast<std::ptrdiff_t>(stop), p,
                                 [](const Level& lv, Price q) { return worse(lv.price, q); });
      i = static_cast<std::size_t>(it - levels_.begin());
    }
    return i;
  }

  std::vector<Level> levels_;  // worst first, best at the back
};

// ---- book and builder ----------------------------------------------------------

template <template <bool> class Levels>
class Book {
 public:
  void add(OrderPool& pool, std::uint32_t idx) {
    Order& o = pool[idx];
    Level& lv = o.side == 'B' ? bids_.get_or_add(o.price) : asks_.get_or_add(o.price);
    o.prev = lv.tail;
    o.next = kNil;
    if (lv.tail != kNil) pool[lv.tail].next = idx;
    else lv.head = idx;
    lv.tail = idx;
    lv.shares += o.shares;
    ++lv.orders;
  }

  void reduce(Order& o, std::uint32_t shares) {
    (o.side == 'B' ? bids_.at(o.price) : asks_.at(o.price)).shares -= shares;
    o.shares -= shares;
  }

  void remove(OrderPool& pool, std::uint32_t idx) {
    if (pool[idx].side == 'B') unlink(bids_, pool, idx);
    else unlink(asks_, pool, idx);
  }

  Top top() const {
    Top t;
    if (const Level* b = bids_.best()) {
      t.bid_px = b->price;
      t.bid_sz = b->shares;
    }
    if (const Level* a = asks_.best()) {
      t.ask_px = a->price;
      t.ask_sz = a->shares;
    }
    return t;
  }

 private:
  template <class Side>
  static void unlink(Side& side, OrderPool& pool, std::uint32_t idx) {
    const Order& o = pool[idx];
    Level& lv = side.at(o.price);
    if (o.prev != kNil) pool[o.prev].next = o.next;
    else lv.head = o.next;
    if (o.next != kNil) pool[o.next].prev = o.prev;
    else lv.tail = o.prev;
    lv.shares -= o.shares;
    if (--lv.orders == 0) side.erase(o.price);
  }

  Levels<true> bids_;
  Levels<false> asks_;
};

// Same interface and semantics as lob::BookBuilder (except the crossed/locked checks), so the
// two can be swapped in the benchmark and compared message by message in the tests.
template <class Index, template <bool> class Levels>
class BookBuilder : public itch::Handler {
 public:
  using itch::Handler::on;
  using BookType = Book<Levels>;

  explicit BookBuilder(const std::vector<std::string>& symbols = {})
      : wanted_(symbols.begin(), symbols.end()), all_symbols_(symbols.empty()) {}

  static std::string name() { return std::string("pool/") + Index::kName + "/" + Levels<true>::kName; }

  void process(const std::uint8_t* msg, std::uint16_t len) {
    touched_ = nullptr;
    itch::dispatch(msg, len, *this);
  }
  const BookType* touched() const { return touched_; }
  std::uint16_t touched_locate() const { return touched_locate_; }
  const BookType* book(std::uint16_t locate) const { return books_[locate].get(); }
  const BookStats& stats() const { return stats_; }
  std::size_t live_orders() const { return live_; }

  void on(const itch::StockDirectory& m) {
    if (!books_[m.locate] && (all_symbols_ || wanted_.count(std::string(m.stock.view()))))
      books_[m.locate] = std::make_unique<BookType>();
  }
  void on(const itch::AddOrder& m) {
    if (BookType* b = books_[m.locate].get()) add(*b, m.locate, m.ref, m.side, m.shares, m.price);
  }
  void on(const itch::OrderExecuted& m) { take_shares(m.locate, m.ref, m.shares); }
  void on(const itch::OrderExecutedWithPrice& m) { take_shares(m.locate, m.ref, m.shares); }
  void on(const itch::OrderCancel& m) { take_shares(m.locate, m.ref, m.shares); }
  void on(const itch::OrderDelete& m) {
    BookType* b = books_[m.locate].get();
    if (!b) return;
    const std::uint32_t idx = index_.find(m.ref);
    if (idx == kNil) {
      ++stats_.unknown_ref;
      return;
    }
    drop(*b, m.ref, idx);
    touch(*b, m.locate);
  }
  void on(const itch::OrderReplace& m) {
    BookType* b = books_[m.locate].get();
    if (!b) return;
    const std::uint32_t idx = index_.find(m.orig_ref);
    if (idx == kNil) {
      ++stats_.unknown_ref;
      return;
    }
    const char side = pool_[idx].side;
    drop(*b, m.orig_ref, idx);
    add(*b, m.locate, m.new_ref, side, m.shares, m.price);
    touch(*b, m.locate);
  }

 private:
  void add(BookType& b, std::uint16_t locate, std::uint64_t ref, char side, std::uint32_t shares, Price price) {
    const std::uint32_t idx = pool_.alloc();
    if (!index_.insert(ref, idx)) {
      pool_.release(idx);
      ++stats_.duplicate_ref;
      return;
    }
    Order& o = pool_[idx];
    o.ref = ref;
    o.price = price;
    o.shares = shares;
    o.locate = locate;
    o.side = side;
    b.add(pool_, idx);
    if (++live_ > stats_.max_live_orders) stats_.max_live_orders = live_;
    touch(b, locate);
  }

  void take_shares(std::uint16_t locate, std::uint64_t ref, std::uint32_t shares) {
    BookType* b = books_[locate].get();
    if (!b) return;
    const std::uint32_t idx = index_.find(ref);
    if (idx == kNil) {
      ++stats_.unknown_ref;
      return;
    }
    Order& o = pool_[idx];
    if (shares < o.shares) {
      b->reduce(o, shares);
    } else {
      if (shares > o.shares) ++stats_.overfill;
      drop(*b, ref, idx);
    }
    touch(*b, locate);
  }

  void drop(BookType& b, std::uint64_t ref, std::uint32_t idx) {
    b.remove(pool_, idx);
    index_.erase(ref);
    pool_.release(idx);
    --live_;
  }

  void touch(BookType& b, std::uint16_t locate) {
    touched_ = &b;
    touched_locate_ = locate;
  }

  OrderPool pool_;
  Index index_;
  std::vector<std::unique_ptr<BookType>> books_ = std::vector<std::unique_ptr<BookType>>(65536);
  std::set<std::string> wanted_;
  bool all_symbols_;
  std::size_t live_ = 0;
  BookType* touched_ = nullptr;
  std::uint16_t touched_locate_ = 0;
  BookStats stats_;
};

}  // namespace lob::fast
