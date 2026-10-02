from pathlib import Path

import pytest

from pylob.book import BookBuilder, top_stream
from pylob.itch import (
    AddOrder,
    OrderCancel,
    OrderDelete,
    OrderExecuted,
    OrderReplace,
    StockDirectory,
)

FIXTURE = Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "sample.itch.gz"
LOC = 7


@pytest.fixture
def b():
    builder = BookBuilder(["TEST"])
    builder.apply(StockDirectory("R", LOC, 0, "TEST", "Q", 100))
    return builder


def add(b, ref, side, shares, price, loc=LOC):
    return b.apply(AddOrder("A", loc, 0, ref, side, shares, "TEST", price, "    "))


def top(b):
    return b.books[LOC].top()


def test_empty_book(b):
    assert top(b) == (0, 0, 0, 0)


def test_best_prices_and_level_totals(b):
    add(b, 1, "B", 100, 10_0000)
    add(b, 2, "B", 200, 10_0100)
    add(b, 3, "B", 50, 10_0100)
    add(b, 4, "S", 300, 10_0300)
    add(b, 5, "S", 100, 10_0200)
    assert top(b) == (10_0100, 250, 10_0200, 100)


def test_executions_and_cancels(b):
    add(b, 1, "S", 100, 10_0000)
    add(b, 2, "S", 100, 10_0100)
    b.apply(OrderExecuted("E", LOC, 0, 1, 40, 1))
    assert top(b) == (0, 0, 10_0000, 60)
    b.apply(OrderCancel("X", LOC, 0, 1, 60))  # cancelling the rest removes the order and its level
    assert top(b) == (0, 0, 10_0100, 100)
    b.apply(OrderDelete("D", LOC, 0, 2))
    assert top(b) == (0, 0, 0, 0)
    assert b.orders == {} and b.stats.overfill == 0


def test_replace_keeps_side(b):
    add(b, 1, "B", 100, 10_0000)
    b.apply(OrderReplace("U", LOC, 0, 1, 10, 300, 10_0500))
    assert top(b) == (10_0500, 300, 0, 0)
    assert set(b.orders) == {10}


def test_integrity_counters(b):
    add(b, 1, "B", 100, 10_0000)
    add(b, 1, "B", 100, 10_0000)  # duplicate ref
    b.apply(OrderExecuted("E", LOC, 0, 1, 150, 1))  # overfill
    b.apply(OrderDelete("D", LOC, 0, 99))  # unknown ref
    assert (b.stats.duplicate_ref, b.stats.overfill, b.stats.unknown_ref) == (1, 1, 1)


def test_untracked_and_repeated_directory(b):
    b.apply(StockDirectory("R", 8, 0, "OTHER", "Q", 100))
    assert add(b, 1, "B", 100, 10_0000, loc=8) is None
    add(b, 2, "B", 100, 10_0000)
    b.apply(StockDirectory("R", LOC, 0, "TEST", "Q", 100))  # repeated: must keep the book
    assert top(b)[1] == 100


@pytest.mark.skipif(not FIXTURE.exists(), reason="fixture not present")
def test_fixture_day_integrity():
    builder = BookBuilder()
    changes = sum(1 for _ in top_stream(FIXTURE, builder=builder))
    s = builder.stats
    assert (s.unknown_ref, s.duplicate_ref, s.overfill, s.crossed, s.locked) == (0, 0, 0, 0, 0)
    assert s.max_live_orders == 877
    assert len(builder.orders) == 0  # every day order is gone by the end of the day
    assert changes == 58_209
