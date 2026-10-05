"""Reference order book (the Python counterpart of lob/book.hpp).

Deliberately simple and independent of the C++ design: orders live in a dict, and each side
of each book is a SortedDict of price -> [total shares, order count]. There are no per-level
queues, because the top of book doesn't depend on queue order. top_stream() emits exactly the
same lines as cpp/apps/build_book, which is how the C++ book is checked.
"""

from dataclasses import dataclass

from sortedcontainers import SortedDict

from .itch import (
    AddOrder,
    OrderCancel,
    OrderDelete,
    OrderExecuted,
    OrderExecutedWithPrice,
    OrderReplace,
    StockDirectory,
    SystemEvent,
    TradingAction,
    decode,
)
from .reader import iter_messages


@dataclass
class BookStats:
    unknown_ref: int = 0
    duplicate_ref: int = 0
    overfill: int = 0
    crossed: int = 0
    locked: int = 0
    max_live_orders: int = 0


class Book:
    def __init__(self):
        self.bids = SortedDict()  # price -> [shares, orders]; best bid is the last key
        self.asks = SortedDict()  # best ask is the first key

    def _side(self, side):
        return self.bids if side == "B" else self.asks

    def add(self, side, price, shares):
        lv = self._side(side).setdefault(price, [0, 0])
        lv[0] += shares
        lv[1] += 1

    def reduce(self, side, price, shares):
        self._side(side)[price][0] -= shares

    def remove(self, side, price, shares):
        levels = self._side(side)
        lv = levels[price]
        lv[0] -= shares
        lv[1] -= 1
        if lv[1] == 0:
            del levels[price]

    def top(self):
        """(bid_px, bid_sz, ask_px, ask_sz); an empty side is (0, 0)."""
        bid_px, bid_sz = (self.bids.peekitem(-1)[0], self.bids.peekitem(-1)[1][0]) if self.bids else (0, 0)
        ask_px, ask_sz = (self.asks.peekitem(0)[0], self.asks.peekitem(0)[1][0]) if self.asks else (0, 0)
        return bid_px, bid_sz, ask_px, ask_sz

    def best_prices(self):
        """(best bid, best ask), or None if either side is empty."""
        if not self.bids or not self.asks:
            return None
        return self.bids.peekitem(-1)[0], self.asks.peekitem(0)[0]


class NaiveBook(Book):
    """Same book with plain dicts: finding the best price scans every level (O(levels)).

    Used only as the slowest point in the Python benchmark.
    """

    def __init__(self):
        self.bids = {}
        self.asks = {}

    def top(self):
        if self.bids:
            bid_px = max(self.bids)
            bid_sz = self.bids[bid_px][0]
        else:
            bid_px = bid_sz = 0
        if self.asks:
            ask_px = min(self.asks)
            ask_sz = self.asks[ask_px][0]
        else:
            ask_px = ask_sz = 0
        return bid_px, bid_sz, ask_px, ask_sz

    def best_prices(self):
        if not self.bids or not self.asks:
            return None
        return max(self.bids), min(self.asks)


class BookBuilder:
    """Applies decoded ITCH messages to one Book per tracked stock."""

    def __init__(self, symbols=(), book_type=Book):
        self.wanted = set(symbols)
        self.book_type = book_type
        self.books = {}  # locate -> Book
        self.symbols = {}  # locate -> symbol
        self.orders = {}  # ref -> [locate, side, price, shares]
        self.trading_state = {}
        self.regular_hours = False
        self.stats = BookStats()

    def apply(self, m):
        """Apply one decoded message. Returns the locate of the book it changed, or None."""
        if isinstance(m, AddOrder):
            book = self.books.get(m.locate)
            if book is not None:
                return self._add(book, m.locate, m.ref, m.side, m.shares, m.price)
        elif isinstance(m, (OrderExecuted, OrderExecutedWithPrice, OrderCancel)):
            book = self.books.get(m.locate)
            if book is not None:
                return self._take_shares(book, m.locate, m.ref, m.shares)
        elif isinstance(m, OrderDelete):
            book = self.books.get(m.locate)
            if book is not None:
                o = self.orders.pop(m.ref, None)
                if o is None:
                    self.stats.unknown_ref += 1
                    return None
                book.remove(o[1], o[2], o[3])
                return self._touch(book, m.locate)
        elif isinstance(m, OrderReplace):
            book = self.books.get(m.locate)
            if book is not None:
                o = self.orders.pop(m.orig_ref, None)
                if o is None:
                    self.stats.unknown_ref += 1
                    return None
                book.remove(o[1], o[2], o[3])
                self._add(book, m.locate, m.new_ref, o[1], m.shares, m.price)
                return self._touch(book, m.locate)
        elif isinstance(m, StockDirectory):
            self.symbols[m.locate] = m.stock
            if m.locate not in self.books and (not self.wanted or m.stock in self.wanted):
                self.books[m.locate] = self.book_type()
        elif isinstance(m, SystemEvent):
            if m.event == "Q":
                self.regular_hours = True
            elif m.event == "M":
                self.regular_hours = False
        elif isinstance(m, TradingAction):
            self.trading_state[m.locate] = m.state
        return None

    def _add(self, book, locate, ref, side, shares, price):
        if ref in self.orders:
            self.stats.duplicate_ref += 1
            return None
        self.orders[ref] = [locate, side, price, shares]
        book.add(side, price, shares)
        self.stats.max_live_orders = max(self.stats.max_live_orders, len(self.orders))
        return self._touch(book, locate)

    def _take_shares(self, book, locate, ref, shares):
        o = self.orders.get(ref)
        if o is None:
            self.stats.unknown_ref += 1
            return None
        if shares < o[3]:
            book.reduce(o[1], o[2], shares)
            o[3] -= shares
        else:
            if shares > o[3]:
                self.stats.overfill += 1
            book.remove(o[1], o[2], o[3])
            del self.orders[ref]
        return self._touch(book, locate)

    def _touch(self, book, locate):
        if self.regular_hours and self.trading_state.get(locate, "T") == "T" and book.bids and book.asks:
            best_bid, best_ask = book.best_prices()
            if best_bid > best_ask:
                self.stats.crossed += 1
            elif best_bid == best_ask:
                self.stats.locked += 1
        return locate


def top_stream(path, symbols=(), builder=None):
    """Yield 'timestamp,locate,bid_px,bid_sz,ask_px,ask_sz' each time a tracked top of book changes."""
    builder = builder if builder is not None else BookBuilder(symbols)
    last = {}
    for buf, off, end in iter_messages(path):
        m = decode(buf, off, end)
        locate = builder.apply(m)
        if locate is None:
            continue
        top = builder.books[locate].top()
        if last.get(locate) != top:
            last[locate] = top
            yield f"{m.timestamp},{locate},{top[0]},{top[1]},{top[2]},{top[3]}"
