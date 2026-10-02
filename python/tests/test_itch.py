from pathlib import Path

import pytest

from pylob import decode, iter_messages
from pylob.itch import LENGTHS, AddOrder, CrossTrade, DecodeError, Other, OrderReplace, StockDirectory, Trade

FIXTURE = Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "sample.itch.gz"
TS = 0x0000123456789ABC  # uses all 6 timestamp bytes


def msg(type_: str, locate: int, ts: int = TS, **fields) -> bytearray:
    """Build a message of the right length with fields at the given byte offsets.

    fields maps "o<offset>" to (value, width) for big-endian integers, or to a str for text.
    """
    b = bytearray(LENGTHS[ord(type_)])
    b[0] = ord(type_)
    b[1:3] = locate.to_bytes(2, "big")
    b[5:11] = ts.to_bytes(6, "big")
    for key, v in fields.items():
        off = int(key[1:])
        if isinstance(v, str):
            b[off : off + len(v)] = v.encode("ascii")
        else:
            value, width = v
            b[off : off + width] = value.to_bytes(width, "big")
    return b


def dec(b: bytearray):
    return decode(b, 0, len(b))


def test_add_order():
    b = msg("A", 4040, o11=(0x0102030405060708, 8), o19="B", o20=(300, 4), o24="IIVI    ", o32=(371_500, 4))
    assert dec(b) == AddOrder("A", 4040, TS, 0x0102030405060708, "B", 300, "IIVI", 371_500, "    ")


def test_add_order_with_attribution():
    b = msg("F", 1, o11=(7, 8), o19="S", o20=(100, 4), o24="FLIR    ", o32=(480_000, 4), o36="GSCO")
    assert dec(b) == AddOrder("F", 1, TS, 7, "S", 100, "FLIR", 480_000, "GSCO")


def test_replace():
    b = msg("U", 3, o11=(42, 8), o19=(43, 8), o27=(500, 4), o31=(99_900, 4))
    assert dec(b) == OrderReplace("U", 3, TS, 42, 43, 500, 99_900)


def test_trade_and_cross():
    p = msg("P", 5, o19="B", o20=(75, 4), o24="AAPL    ", o32=(1_650_000, 4), o36=(777, 8))
    assert dec(p) == Trade("P", 5, TS, 0, "B", 75, "AAPL", 1_650_000, 777)
    q = msg("Q", 5, o11=(5_000_000_000, 8), o19="AAPL    ", o27=(1_651_000, 4), o31=(778, 8), o39="O")
    assert dec(q) == CrossTrade("Q", 5, TS, 5_000_000_000, "AAPL", 1_651_000, 778, "O")


def test_directory_and_other():
    r = msg("R", 13, o11="QQQ     ", o19="G", o21=(100, 4))
    assert dec(r) == StockDirectory("R", 13, TS, "QQQ", "G", 100)
    assert dec(msg("I", 13)) == Other("I", 13, TS)


def test_wrong_length_raises():
    b = msg("D", 1) + b"\x00"
    with pytest.raises(DecodeError):
        dec(b)


def test_unknown_type_raises():
    with pytest.raises(DecodeError):
        dec(bytearray(b"Z" + bytes(11)))


@pytest.mark.skipif(not FIXTURE.exists(), reason="fixture not present")
def test_fixture_fields_are_consistent():
    directory = {}
    last_ts = 0
    adds = trades = 0
    for buf, off, end in iter_messages(FIXTURE):
        m = decode(buf, off, end)
        assert m.timestamp >= last_ts
        last_ts = m.timestamp
        if isinstance(m, StockDirectory):
            directory[m.locate] = m.stock
        elif isinstance(m, AddOrder):
            adds += 1
            assert directory[m.locate] == m.stock
            assert m.side in "BS" and m.shares > 0
        elif isinstance(m, Trade):
            trades += 1
            assert directory[m.locate] == m.stock
            # FLIR traded near $48 and IIVI near $37 on 2019-01-30.
            assert 20 * 10_000 <= m.price <= 100 * 10_000
    assert adds > 60_000 and trades > 500
