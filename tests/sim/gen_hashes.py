"""Regenerate hashes.h (H_<Name> for each wrapper in gta/src/natives.h) for the simulator's native dispatcher."""
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
src = (HERE.parent.parent / "gta" / "src" / "natives.h").read_text()
out = ["#pragma once"]
seen = set()
for m in re.finditer(r"inline [^(]*?\b(\w+)\([^)]*\)\s*(?:\{[^}]*?|\n\s*\{[^}]*?)invoke<[^>]*>\((0x[0-9A-Fa-f]{16})", src):
    name, h = m.group(1), m.group(2).upper().replace("0X", "0x")
    if name not in seen:
        seen.add(name)
        out.append(f"#define H_{name} {h}ull")
(HERE / "hashes.h").write_text("\n".join(out) + "\n")
print(len(seen), "hashes")
