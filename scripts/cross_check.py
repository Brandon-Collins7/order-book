"""Check that the C++ and Python implementations produce identical output.

  python scripts/cross_check.py dump <itch_dump exe>  <file.gz>
      every decoded field of every message (itch_dump vs pylob.dump_line)
  python scripts/cross_check.py book <build_book exe> <file.gz> [SYMBOL ...]
      the top-of-book stream (build_book vs pylob.book.top_stream)

Compares line by line and exits 1 at the first difference.
"""

import subprocess
import sys
from itertools import zip_longest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python"))
from pylob import decode, dump_line, iter_messages  # noqa: E402
from pylob.book import top_stream  # noqa: E402


def main():
    if len(sys.argv) < 4 or sys.argv[1] not in ("dump", "book"):
        sys.exit(__doc__)
    mode, exe, path, symbols = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]

    if mode == "dump":
        cmd = [exe, path]
        py_lines = (dump_line(decode(buf, off, end)) for buf, off, end in iter_messages(path))
    else:
        cmd = [exe, path, *symbols]
        py_lines = top_stream(path, symbols)

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    n = 0
    for n, (cpp, py) in enumerate(zip_longest(proc.stdout, py_lines), start=1):
        cpp = cpp.rstrip("\r\n") if cpp is not None else None
        if cpp != py:
            proc.kill()
            print(f"mismatch at line {n}:\n  C++:    {cpp}\n  Python: {py}")
            sys.exit(1)
    if proc.wait() != 0:
        sys.exit(f"{Path(exe).name} exited with code {proc.returncode}")
    what = "messages" if mode == "dump" else "top-of-book changes"
    print(f"C++ and Python agree on all {n:,} {what}")


if __name__ == "__main__":
    main()
