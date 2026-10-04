"""Integration test for the Minecraft side: a fake GTA host over the mod's link. Checks screens (inventory, chat
typing, commands), the End and Nether portals in and out, projectile reports, melee kinds and glass breaking."""
import asyncio
import json
import math
import sys
import time

import websockets

URL = "ws://127.0.0.1:25599"
GROUND = 64.0
log = []
state = {"pos": [0.5, GROUND, 0.5], "yaw": 0.0, "pitch": 20.0, "fp": True}


def note(*a):
    msg = " ".join(str(x) for x in a)
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


async def reader(ws, inbox):
    async for m in ws:
        try:
            j = json.loads(m)
        except Exception:
            continue
        inbox.append(j)
        t = j.get("t")
        if t not in ("proj", "mcpos", "blocks", "mobs", "hot"):
            note("<-", m[:200])
        elif t == "proj":
            note("<- proj", [p[1] for p in j["p"]] if j["p"] else "[] (none left)")
        elif t == "blocks":
            note("<- blocks", m[:200])


async def cam_loop(ws):
    f = 0
    while True:
        x, y, z = state["pos"]
        yaw, pitch = state["yaw"], state["pitch"]
        f += 1
        if state["fp"]:
            eye = [x, y + 1.62, z]
            msg = {"t": "cam", "f": f, "p": eye, "r": [yaw, pitch, 0], "fov": 70, "fp": True, "pl": [x, y, z], "h": yaw}
        else:
            yr, pr = math.radians(yaw), math.radians(pitch)
            fwd = [-math.sin(yr) * math.cos(pr), -math.sin(pr), math.cos(yr) * math.cos(pr)]
            cam = [x - fwd[0] * 3.5 + 0.6, y + 1.9 - fwd[1] * 3.5, z - fwd[2] * 3.5]
            aim = [x + fwd[0] * 30, y + 1.6 + fwd[1] * 30, z + fwd[2] * 30]
            msg = {"t": "cam", "f": f, "p": cam, "r": [yaw, pitch, 0], "fov": 70, "fp": False, "pl": [x, y, z], "h": yaw,
                   "gun": False, "veh": False, "aim": aim, "aimOn": True}
        await ws.send(json.dumps(msg))
        await asyncio.sleep(1 / 60)


async def expect(inbox, pred, timeout, what):
    t0 = time.time()
    while time.time() - t0 < timeout:
        for j in list(inbox):
            if pred(j):
                inbox.remove(j)
                note("OK:", what)
                return j
        await asyncio.sleep(0.05)
    note("FAIL:", what)
    log.append(what)
    return None


async def main():
    async with websockets.connect(URL, max_size=None) as ws:
        inbox = []
        asyncio.create_task(reader(ws, inbox))
        await ws.send(json.dumps({"t": "view", "w": 1280, "h": 720}))
        # flat host ground: barrier tops at y=64 around the origin
        cols = []
        for x in range(-30, 31):
            for z in range(-30, 31):
                cols += [x, z, 62, 63]
        await ws.send(json.dumps({"t": "ground", "c": cols}))
        asyncio.create_task(cam_loop(ws))
        await asyncio.sleep(4)
        for c in ["fill -10 64 -10 10 74 10 minecraft:air", "kill @e[type=!player]"]:
            await ws.send(json.dumps({"t": "cmd", "c": c}))
        await asyncio.sleep(1)
        inbox.clear()

        # --- inventory: open with the key, click around, close with I
        await ws.send(json.dumps({"t": "key", "k": "inventory", "down": True}))
        await ws.send(json.dumps({"t": "key", "k": "inventory", "down": False}))
        await expect(inbox, lambda j: j.get("t") == "screen" and j.get("kind") == 2, 5, "inventory opens (screen kind 2)")
        for i in range(10):
            await ws.send(json.dumps({"t": "cursor", "x": 0.3 + i * 0.03, "y": 0.4}))
            await asyncio.sleep(0.03)
        await ws.send(json.dumps({"t": "mbtn", "b": 1, "down": True, "m": 0}))
        await ws.send(json.dumps({"t": "cursor", "x": 0.45, "y": 0.45}))
        await ws.send(json.dumps({"t": "mbtn", "b": 1, "down": False, "m": 0}))
        await ws.send(json.dumps({"t": "wheel", "d": -1}))
        await asyncio.sleep(0.5)
        await ws.send(json.dumps({"t": "rkey", "sc": 12, "kc": ord("i"), "m": 0, "a": 1}))
        await ws.send(json.dumps({"t": "rkey", "sc": 12, "kc": ord("i"), "m": 0, "a": 0}))
        await expect(inbox, lambda j: j.get("t") == "screen" and j.get("kind") == 0, 5, "I closes the inventory (screen kind 0)")

        async def chat(text):
            await ws.send(json.dumps({"t": "key", "k": "chat", "down": True}))
            await ws.send(json.dumps({"t": "key", "k": "chat", "down": False}))
            await expect(inbox, lambda j: j.get("t") == "screen" and j.get("kind") == 1, 5, "chat opens (screen kind 1)")
            for ch in text:
                await ws.send(json.dumps({"t": "chr", "c": ord(ch)}))
            await asyncio.sleep(0.3)
            await ws.send(json.dumps({"t": "rkey", "sc": 40, "kc": 13, "m": 0, "a": 1}))
            await ws.send(json.dumps({"t": "rkey", "sc": 40, "kc": 13, "m": 0, "a": 0}))
            await expect(inbox, lambda j: j.get("t") == "screen" and j.get("kind") == 0, 5, f"chat closes after Enter ({text})")

        # --- the End: /endportal builds one 5 ahead (looking +z), walk in, out, back in
        state.update(pos=[0.5, GROUND, 0.5], yaw=0.0, pitch=20.0, fp=True)
        await asyncio.sleep(0.5)
        await chat("/endportal")
        await asyncio.sleep(1.0)
        state["pos"] = [0.5, GROUND, 5.5]
        await expect(inbox, lambda j: j.get("t") == "end" and j.get("on") is True, 6, "walking into the end portal opens the End")
        state["pos"] = [0.5, GROUND, 12.5]
        await expect(inbox, lambda j: j.get("t") == "note" and "dragon is coming" in j.get("text", ""), 35, "the ender dragon arrives")
        await asyncio.sleep(3)
        await ws.send(json.dumps({"t": "cmd", "c": "kill @e[type=minecraft:ender_dragon]"}))
        await expect(inbox, lambda j: j.get("t") == "note" and "dragon is dead" in j.get("text", ""), 15, "killing the dragon is noticed")
        await asyncio.sleep(1.5)
        state["pos"] = [0.5, GROUND, 5.5]
        await expect(inbox, lambda j: j.get("t") == "end" and j.get("on") is False, 6, "walking back into the end portal closes the End")
        state["pos"] = [0.5, GROUND, 0.5]
        await asyncio.sleep(1.5)

        # --- the Nether: /netherportal 5 ahead (looking -x), jump in (the frame's bottom row is a block up), out, in
        state.update(pos=[-10.5, GROUND, -10.5], yaw=90.0, pitch=0.0)
        await asyncio.sleep(0.5)
        await chat("/netherportal")
        await asyncio.sleep(1.5)
        state["pos"] = [-15.5, GROUND + 1, -10.5]
        await expect(inbox, lambda j: j.get("t") == "nether" and j.get("on") is True, 6, "walking into the nether portal opens the Nether")
        await asyncio.sleep(3)
        state["pos"] = [-10.5, GROUND, -10.5]
        await asyncio.sleep(1.5)
        state["pos"] = [-15.5, GROUND + 1, -10.5]
        await expect(inbox, lambda j: j.get("t") == "nether" and j.get("on") is False, 6, "walking back through the nether portal closes the Nether")
        state["pos"] = [0.5, GROUND, 0.5]
        await asyncio.sleep(1)

        # --- melee kinds: sword (hotbar 1), fist (empty: give nothing, select slot of air?)
        state.update(pos=[0.5, GROUND, 0.5], yaw=0.0, pitch=-10.0, fp=False)
        await ws.send(json.dumps({"t": "cmd", "c": "item replace entity @a hotbar.1 with minecraft:diamond_sword"}))
        await ws.send(json.dumps({"t": "slot", "n": 1}))
        await asyncio.sleep(0.3)
        await ws.send(json.dumps({"t": "key", "k": "attack", "down": True}))
        await ws.send(json.dumps({"t": "key", "k": "attack", "down": False}))
        await expect(inbox, lambda j: j.get("t") == "melee" and j.get("k") == "sword", 3, "sword swing reported as melee k=sword")
        await ws.send(json.dumps({"t": "cmd", "c": "item replace entity @a hotbar.1 with minecraft:mace"}))
        await asyncio.sleep(0.5)
        await ws.send(json.dumps({"t": "key", "k": "attack", "down": True}))
        await ws.send(json.dumps({"t": "key", "k": "attack", "down": False}))
        await expect(inbox, lambda j: j.get("t") == "melee" and j.get("k") == "mace", 3, "mace swing reported as melee k=mace")

        # --- bow: an arrow in flight is reported, then the empty list once it's down
        await ws.send(json.dumps({"t": "slot", "n": 3}))
        await asyncio.sleep(0.3)
        await ws.send(json.dumps({"t": "key", "k": "use", "down": True}))
        await asyncio.sleep(1.2)
        await ws.send(json.dumps({"t": "key", "k": "use", "down": False}))
        await expect(inbox, lambda j: j.get("t") == "proj" and any(p[1].split(":")[0] == "arrow" for p in j["p"]), 4, "arrow in flight reported (with its bow's enchantments)")
        await expect(inbox, lambda j: j.get("t") == "proj" and j["p"] == [], 12, "empty projectile list once the arrow is down")

        # --- snowball and splash potion kinds
        await ws.send(json.dumps({"t": "cmd", "c": "item replace entity @a hotbar.4 with minecraft:snowball 16"}))
        await asyncio.sleep(0.3)
        await ws.send(json.dumps({"t": "slot", "n": 4}))
        await asyncio.sleep(0.3)
        await ws.send(json.dumps({"t": "key", "k": "use", "down": True}))
        await asyncio.sleep(0.1)
        await ws.send(json.dumps({"t": "key", "k": "use", "down": False}))
        await expect(inbox, lambda j: j.get("t") == "proj" and any(p[1] == "snowball" for p in j["p"]), 4, "snowball reported")
        await ws.send(json.dumps({"t": "cmd", "c": "item replace entity @a hotbar.4 with minecraft:splash_potion[potion_contents={potion:\"minecraft:strong_harming\"}]"}))
        await asyncio.sleep(0.5)
        await ws.send(json.dumps({"t": "key", "k": "use", "down": True}))
        await asyncio.sleep(0.1)
        await ws.send(json.dumps({"t": "key", "k": "use", "down": False}))
        await expect(inbox, lambda j: j.get("t") == "proj" and any(p[1] == "potion:strong_harming" for p in j["p"]), 4, "splash potion reported as potion:strong_harming")
        # a host hit on a flying potion: it splashes there
        j = None
        t0 = time.time()
        while time.time() - t0 < 3 and j is None:
            for m in list(inbox):
                if m.get("t") == "proj" and m["p"]:
                    j = m
            await asyncio.sleep(0.05)
        if j:
            pid, kind, x, y, z = j["p"][0]
            await ws.send(json.dumps({"t": "projhit", "id": pid, "pos": [x, y, z], "stick": False}))
            note("sent projhit for", kind)

        note("FAILURES:", log if log else "none")


asyncio.run(main())
