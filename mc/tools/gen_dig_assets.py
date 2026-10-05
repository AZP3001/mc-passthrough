"""Writes the resources for the pieces of the host's world mined with a pickaxe (HostSurfaceBlock): run from mc/.

A clear texture (the piece is unseen), a model per depth (the part of the cell behind the host's surface, the surface on
top) and crack stage (none, or Minecraft's destroy stages 0..9 as its faces: they show over the host's wall, which
Minecraft's own cracks, darkening what's under them, don't) turned to each facing, the loot each material drops, the
pickaxe's tag and the names. Kinds: rock, soil, sand, wood, metal (HostDig.KINDS)."""
import json
import os
import shutil
import struct
import zlib

KINDS = {
    # kind: (particle texture, drop, count, name)
    "rock": ("minecraft:block/stone", "minecraft:cobblestone", 1, "GTA Rock"),
    "soil": ("minecraft:block/dirt", "minecraft:dirt", 1, "GTA Ground"),
    "sand": ("minecraft:block/sand", "minecraft:sand", 1, "GTA Sand"),
    "wood": ("minecraft:block/oak_planks", "minecraft:oak_planks", 1, "GTA Wood"),
    "metal": ("minecraft:block/iron_block", "minecraft:iron_nugget", 3, "GTA Metal"),
}
# the model is made facing up (the surface on top); turned as Minecraft turns its lightning rod
TURN = {"up": {}, "down": {"x": 180}, "north": {"x": 90}, "south": {"x": 90, "y": 180}, "east": {"x": 90, "y": 90}, "west": {"x": 90, "y": 270}}
ROOT = "src/main/resources"


def write(path, data):
    path = os.path.join(ROOT, path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if isinstance(data, (dict, list)):
        data = json.dumps(data, separators=(",", ":")) + "\n"
    with open(path, "w" if isinstance(data, str) else "wb") as f:
        f.write(data)


def clear_png(size=16):
    raw = b"".join(b"\x00" + b"\x00\x00\x00\x00" * size for _ in range(size))
    chunk = lambda tag, body: struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


shutil.rmtree(os.path.join(ROOT, "assets/passthrough/models/block"), ignore_errors=True)
write("assets/passthrough/textures/block/clear.png", clear_png())
lang = {}
pickaxe = []
for kind, (particle, drop, count, name) in KINDS.items():
    block = f"gta_{kind}"
    pickaxe.append(f"passthrough:{block}")
    lang[f"block.passthrough.{block}"] = name
    variants = {}
    for depth in range(16):
        for crack in range(11):
            face = {"texture": "#face"}
            look = "passthrough:block/clear" if crack == 0 else f"minecraft:block/destroy_stage_{crack - 1}"
            write(f"assets/passthrough/models/block/{block}_{depth}_{crack}.json", {
                "textures": {"particle": particle, "face": look},
                "elements": [{"from": [0, 0, 0], "to": [16, 16 - depth, 16],
                              "faces": {d: dict(face) for d in ("down", "up", "north", "south", "west", "east")}}],
            })
            for facing, turn in TURN.items():
                variants[f"crack={crack},depth={depth},facing={facing}"] = {"model": f"passthrough:block/{block}_{depth}_{crack}", **turn}
    write(f"assets/passthrough/blockstates/{block}.json", {"variants": variants})
    entry = {"type": "minecraft:item", "name": drop, "condition": {"type": "minecraft:survives_explosion"}}
    if count > 1:
        entry["functions"] = [{"function": "minecraft:set_count", "count": {"type": "minecraft:uniform", "min": 1, "max": count}}]
    write(f"data/passthrough/loot_table/blocks/{block}.json", {
        "type": "minecraft:block", "pools": [{"rolls": 1, "entries": [entry]}], "random_sequence": f"passthrough:blocks/{block}"})
write("data/minecraft/tags/block/mineable/pickaxe.json", {"replace": False, "values": pickaxe})
write("assets/passthrough/lang/en_us.json", lang)
print("written")
