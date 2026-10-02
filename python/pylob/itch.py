"""Decoding of Nasdaq TotalView-ITCH 5.0 messages (the Python counterpart of lob/itch.hpp).

Each message type has a big-endian struct format. The 6-byte timestamp is read as a 2-byte
high part and a 4-byte low part. Decoded messages are NamedTuples whose fields come in the
same order as the C++ structs, so dump_line() gives exactly the same text as cpp/apps/itch_dump.
Prices stay as integers in units of 1/10,000 of a dollar.
"""

import struct
from typing import NamedTuple

PRICE_SCALE = 10_000

# Expected total length of each message type, including the type byte.
LENGTHS = {
    ord(t): n
    for t, n in {
        "S": 12, "R": 39, "H": 25, "Y": 20, "L": 26, "V": 35, "W": 12, "K": 28, "J": 35, "h": 21,
        "A": 36, "F": 40, "E": 31, "C": 36, "X": 23, "D": 19, "U": 35, "P": 44, "Q": 40, "B": 19,
        "I": 50, "N": 20, "O": 48,
    }.items()
}


class DecodeError(ValueError):
    pass


class SystemEvent(NamedTuple):
    type: str
    locate: int
    timestamp: int
    event: str


class StockDirectory(NamedTuple):
    type: str
    locate: int
    timestamp: int
    stock: str
    market_category: str
    round_lot_size: int


class TradingAction(NamedTuple):
    type: str
    locate: int
    timestamp: int
    stock: str
    state: str


class AddOrder(NamedTuple):  # 'A' and 'F'
    type: str
    locate: int
    timestamp: int
    ref: int
    side: str
    shares: int
    stock: str
    price: int
    mpid: str


class OrderExecuted(NamedTuple):
    type: str
    locate: int
    timestamp: int
    ref: int
    shares: int
    match: int


class OrderExecutedWithPrice(NamedTuple):
    type: str
    locate: int
    timestamp: int
    ref: int
    shares: int
    match: int
    printable: str
    price: int


class OrderCancel(NamedTuple):
    type: str
    locate: int
    timestamp: int
    ref: int
    shares: int


class OrderDelete(NamedTuple):
    type: str
    locate: int
    timestamp: int
    ref: int


class OrderReplace(NamedTuple):
    type: str
    locate: int
    timestamp: int
    orig_ref: int
    new_ref: int
    shares: int
    price: int


class Trade(NamedTuple):
    type: str
    locate: int
    timestamp: int
    ref: int
    side: str
    shares: int
    stock: str
    price: int
    match: int


class CrossTrade(NamedTuple):
    type: str
    locate: int
    timestamp: int
    shares: int
    stock: str
    price: int
    match: int
    cross_type: str


class BrokenTrade(NamedTuple):
    type: str
    locate: int
    timestamp: int
    match: int


class Other(NamedTuple):
    type: str
    locate: int
    timestamp: int


def _c(b: bytes) -> str:
    return b.decode("ascii")


def _sym(b: bytes) -> str:
    return b.decode("ascii").rstrip(" ")


# type byte -> (struct format after the 11-byte header, builder taking (type, locate, ts, fields))
_HEADER = ">cHHHI"  # type, locate, tracking, timestamp high 16 bits, timestamp low 32 bits
_SPECS = {
    "S": ("c", lambda h, f: SystemEvent(*h, _c(f[0]))),
    "R": ("8sccI14x", lambda h, f: StockDirectory(*h, _sym(f[0]), _c(f[1]), f[3])),
    "H": ("8sc5x", lambda h, f: TradingAction(*h, _sym(f[0]), _c(f[1]))),
    "A": ("QcI8sI", lambda h, f: AddOrder(*h, f[0], _c(f[1]), f[2], _sym(f[3]), f[4], "    ")),
    "F": ("QcI8sI4s", lambda h, f: AddOrder(*h, f[0], _c(f[1]), f[2], _sym(f[3]), f[4], _c(f[5]))),
    "E": ("QIQ", lambda h, f: OrderExecuted(*h, *f)),
    "C": ("QIQcI", lambda h, f: OrderExecutedWithPrice(*h, f[0], f[1], f[2], _c(f[3]), f[4])),
    "X": ("QI", lambda h, f: OrderCancel(*h, *f)),
    "D": ("Q", lambda h, f: OrderDelete(*h, *f)),
    "U": ("QQII", lambda h, f: OrderReplace(*h, *f)),
    "P": ("QcI8sIQ", lambda h, f: Trade(*h, f[0], _c(f[1]), f[2], _sym(f[3]), f[4], f[5])),
    "Q": ("Q8sIQc", lambda h, f: CrossTrade(*h, f[0], _sym(f[1]), f[2], f[3], _c(f[4]))),
    "B": ("Q", lambda h, f: BrokenTrade(*h, *f)),
}
_STRUCTS = {ord(t): (struct.Struct(_HEADER + fmt), build) for t, (fmt, build) in _SPECS.items()}
_HEADER_STRUCT = struct.Struct(_HEADER)

for _t, (_s, _) in _STRUCTS.items():
    assert _s.size == LENGTHS[_t], (chr(_t), _s.size, LENGTHS[_t])


def decode(buf, off: int, end: int):
    """Decode the message at buf[off:end] (buf[off] is the type byte)."""
    t = buf[off]
    want = LENGTHS.get(t)
    if want is None:
        raise DecodeError(f"unknown message type {chr(t)!r}")
    if end - off != want:
        raise DecodeError(f"message {chr(t)!r} has length {end - off}, expected {want}")
    spec = _STRUCTS.get(t)
    if spec is None:
        _, locate, _, hi, lo = _HEADER_STRUCT.unpack_from(buf, off)
        return Other(chr(t), locate, (hi << 32) | lo)
    s, build = spec
    v = s.unpack_from(buf, off)
    return build((chr(t), v[1], (v[3] << 32) | v[4]), v[5:])


def dump_line(msg) -> str:
    """One comma-separated line per message, identical to cpp/apps/itch_dump."""
    return ",".join(str(x) for x in msg)
