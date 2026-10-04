"""Check every native called in gta/src (invoke<T>(0xHASH, args...)) against alloc8or's native DB: the hash must
exist, and the number of arguments must match the native's parameter count.

    python3 gta/tests/check_natives.py path/to/natives.json

natives.json: https://raw.githubusercontent.com/alloc8or/gta5-nativedb-data/master/natives.json
"""
import json
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "src"


def split_args(s):
    """Top-level comma split of an argument list."""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "([{<":
            depth += 1
        elif ch in ")]}>":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def calls(text):
    """(hash, argc, line) for each invoke<...>(0x..., ...)."""
    for m in re.finditer(r"invoke<[^>]*>\(\s*(0x[0-9A-Fa-f]{16})", text):
        start = m.end()
        depth, i = 1, start
        while depth and i < len(text):
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
            i += 1
        inner = text[start:i - 1].strip()
        if inner.startswith(","):
            inner = inner[1:]
        args = split_args(inner) if inner.strip() else []
        yield m.group(1).upper().replace("0X", "0x"), len(args), text.count("\n", 0, m.start()) + 1


def main():
    db = json.load(open(sys.argv[1] if len(sys.argv) > 1 else "natives.json"))
    natives = {}
    for ns, entries in db.items():
        for h, n in entries.items():
            natives[h.upper().replace("0X", "0x")] = (ns, n)
    problems = total = 0
    for f in sorted(SRC.glob("*.[ch]*")):
        for h, argc, line in calls(f.read_text()):
            total += 1
            if h not in natives:
                print(f"{f.name}:{line}: {h} not in the DB")
                problems += 1
                continue
            ns, n = natives[h]
            want = len(n.get("params", []))
            if want != argc:
                print(f"{f.name}:{line}: {ns}::{n['name']} takes {want} args, called with {argc}")
                problems += 1
    print(f"{total} calls, {problems} problems")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
