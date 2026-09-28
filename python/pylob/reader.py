"""Streaming reader for gzipped ITCH 5.0 files (the Python counterpart of lob::GzItchReader)."""

import gzip

CHUNK = 1 << 26  # 64 MiB of decompressed data per read


def iter_messages(path, chunk=CHUNK):
    """Yield (buf, offset, end) for each framed message in a gzipped ITCH file.

    buf[offset] is the message type byte and the message runs to buf[end]. The buffer is
    reused across messages, so slice it (buf[offset:end]) if you need to keep the bytes.
    A message that straddles a chunk boundary is carried over into the next chunk.
    """
    with gzip.open(path, "rb") as f:
        tail = b""
        while True:
            data = f.read(chunk)
            if not data:
                break
            buf = tail + data
            n = len(buf)
            off = 0
            while off + 2 <= n:
                end = off + 2 + ((buf[off] << 8) | buf[off + 1])
                if end > n:
                    break
                yield buf, off + 2, end
                off = end
            tail = buf[off:]
        if tail:
            raise ValueError(f"{len(tail)} trailing bytes do not form a complete message")
