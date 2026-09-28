import gzip
from collections import Counter
from pathlib import Path

import pytest

from pylob import iter_messages

FIXTURES = Path(__file__).resolve().parents[2] / "tests" / "fixtures"


def frame(body: bytes) -> bytes:
    return len(body).to_bytes(2, "big") + body


def patterned_body(i: int) -> bytes:
    return bytes((i + k) % 251 for k in range(100 + i % 37))


def test_messages_straddling_chunks_arrive_intact(tmp_path):
    bodies = [patterned_body(i) for i in range(500)]
    path = tmp_path / "straddle.gz"
    with gzip.open(path, "wb") as f:
        f.write(b"".join(frame(b) for b in bodies))

    got = [bytes(buf[off:end]) for buf, off, end in iter_messages(path, chunk=333)]
    assert got == bodies


def test_truncated_file_raises(tmp_path):
    path = tmp_path / "truncated.gz"
    with gzip.open(path, "wb") as f:
        f.write(frame(b"S\x01\x02\x03") + b"\x00\x0aA")  # second message declares 10 bytes, has 1
    with pytest.raises(ValueError):
        list(iter_messages(path))


def test_empty_file_has_no_messages(tmp_path):
    path = tmp_path / "empty.gz"
    with gzip.open(path, "wb"):
        pass
    assert list(iter_messages(path)) == []


@pytest.mark.skipif(not (FIXTURES / "sample.itch.gz").exists(), reason="fixture not present")
def test_fixture_counts_match_slicer():
    want = {}
    for line in (FIXTURES / "sample.counts.txt").read_text().splitlines():
        key, value = line.split()
        want[key] = int(value)

    got = Counter()
    events = []
    for buf, off, end in iter_messages(FIXTURES / "sample.itch.gz"):
        t = chr(buf[off])
        got[t] += 1
        if t == "S":
            assert end - off == 12
            events.append(chr(buf[off + 11]))
    got["total"] = sum(got.values())

    assert dict(got) == want
    assert events[0] == "O" and events[-1] == "C"
