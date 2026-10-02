"""Check that the C++ and Python decoders agree on every field of every message.

Runs cpp/apps/itch_dump on an ITCH file and compares its output line by line with
pylob.dump_line over the same file. Exits 1 and prints the first difference on a mismatch.

  python scripts/cross_check_dump.py build/release/itch_dump tests/fixtures/sample.itch.gz
"""

import subprocess
import sys
from itertools import zip_longest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python"))
from pylob import decode, dump_line, iter_messages  # noqa: E402


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    exe, path = sys.argv[1], sys.argv[2]

    proc = subprocess.Popen([exe, path], stdout=subprocess.PIPE, text=True)
    py_lines = (dump_line(decode(buf, off, end)) for buf, off, end in iter_messages(path))

    n = 0
    for n, (cpp, py) in enumerate(zip_longest(proc.stdout, py_lines), start=1):
        cpp = cpp.rstrip("\r\n") if cpp is not None else None
        if cpp != py:
            proc.kill()
            print(f"mismatch at message {n}:\n  C++:    {cpp}\n  Python: {py}")
            sys.exit(1)
    if proc.wait() != 0:
        sys.exit(f"itch_dump exited with code {proc.returncode}")
    print(f"C++ and Python decoders agree on all {n:,} messages")


if __name__ == "__main__":
    main()
