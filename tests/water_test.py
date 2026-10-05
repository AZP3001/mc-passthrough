"""Integration test for the host's water as Minecraft's (run with Minecraft running, no GTA): Minecraft's player in the
host's water is in water (it swims there), standing in the host's water both while the host places it and while
Minecraft moves it; and a riptide trident launches there. Prints OK/FAIL lines and a summary.

Each check runs a command that summons an animal only if the check holds: the mod turns it into one of the host's
("animal"), which is what the test waits for."""
import asyncio
import json
import time

import websockets

URL = "ws://127.0.0.1:25599"
fails = []
IN_WATER = '{type:"minecraft:entity_properties",entity:"this",predicate:{flags:{is_in_water:true}}}'


def note(*a):
    print(f"[{time.strftime('%H:%M:%S')}] " + " ".join(str(x) for x in a), flush=True)


async def reader(ws, inbox):
    async for m in ws:
        try:
            inbox.append(json.loads(m))
        except Exception:
            pass


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
    fails.append(what)
    return None


async def cam_loop(ws, state):
    f = 0
    while True:
        x, y, z = state["pos"]
        f += 1
        await ws.send(json.dumps({"t": "cam", "f": f, "p": [x, y + 1.62, z], "r": [0, 20, 0], "fov": 70, "fp": True, "pl": [x, y, z], "h": 0,
                                  "ctl": True, "hp": [100, 100], "ar": 0, "st": 100.0, "walk": state["walk"], "in": 0}))
        await asyncio.sleep(1 / 60)


async def main():
    # the host's ground (barriers up to y 63) all round, its sea floor lower (up to y 57) where its water is: x 0..7,
    # z -4..3, from y 58 up to a surface at y 66
    state = {"pos": [-10.5, 64.0, 0.5], "walk": False}
    async with websockets.connect(URL, max_size=None) as ws:
        inbox = []
        asyncio.create_task(reader(ws, inbox))
        await ws.send(json.dumps({"t": "clear"}))
        await asyncio.sleep(0.3)
        cols, water = [], []
        for x in range(-20, 21):
            for z in range(-20, 21):
                sea = 0 <= x <= 7 and -4 <= z <= 3
                cols += [x, z, 56 if sea else 62, 57 if sea else 63]
                if sea:
                    water += [x, z, 58, 66 * 256]
        await ws.send(json.dumps({"t": "ground", "c": cols}))
        await ws.send(json.dumps({"t": "gwater", "c": water}))
        # the host's collision for the player Minecraft moves: its sea floor (top y 58) under the water
        await ws.send(json.dumps({"t": "hc", "b": [0, 57, -4, 8, 58, 4]}))
        asyncio.create_task(cam_loop(ws, state))
        await asyncio.sleep(3)

        async def cmd(*cs):
            for c in cs:
                await ws.send(json.dumps({"t": "cmd", "c": c}))
            await asyncio.sleep(0.4)

        async def check(condition, animal, model, what):
            # summons `animal` (turned into the host's `model`) only if the execute condition holds
            inbox[:] = [j for j in inbox if j.get("t") != "animal"]
            await cmd(f"execute {condition} run summon minecraft:{animal} -8 64 -8")
            await expect(inbox, lambda j: j.get("t") == "animal" and j.get("m") == model, 3, what)

        await cmd("kill @e[type=!player]", "gamemode creative @a", "effect give @a minecraft:water_breathing infinite 0 true")
        await asyncio.sleep(1)
        inbox.clear()

        # on the host's dry ground: not in water
        await check(f"as @p unless predicate {IN_WATER}", "cow", "a_c_cow", "on the host's dry ground: not in water")
        # the host puts its player in its water (GTA's own movement): in water
        state["pos"] = [3.5, 60.0, 0.5]
        await asyncio.sleep(1.0)
        await check(f"as @p if predicate {IN_WATER}", "chicken", "a_c_hen", "the host's player in its water: in water in Minecraft")
        # Minecraft moves the player (Minecraft movement): in water, and it sinks/swims there rather than standing
        state["walk"] = True
        await asyncio.sleep(1.5)
        await check(f"as @p if predicate {IN_WATER}", "pig", "a_c_pig", "Minecraft moving its player in the host's water: in water")
        await check("as @p at @s if entity @s[y=57,dy=8.5]", "rabbit", "a_c_rabbit_01", "and it stays in the water (no floor at the surface)")
        state["walk"] = False
        state["pos"] = [3.5, 60.0, 0.5]
        await asyncio.sleep(1.0)

        # a riptide trident in the host's water: it launches (the host is told)
        await cmd('item replace entity @a weapon.mainhand with minecraft:trident[enchantments={"minecraft:riptide":3}]')
        await asyncio.sleep(0.5)
        inbox.clear()
        await ws.send(json.dumps({"t": "key", "k": "use", "down": True}))
        await asyncio.sleep(0.9)
        await ws.send(json.dumps({"t": "key", "k": "use", "down": False}))
        await expect(inbox, lambda j: j.get("t") == "riptide", 3, "a riptide trident launches in the host's water")
        await asyncio.sleep(1.0)
        # and not on the host's dry ground
        state["pos"] = [-10.5, 64.0, 0.5]
        await asyncio.sleep(1.0)
        inbox.clear()
        await ws.send(json.dumps({"t": "key", "k": "use", "down": True}))
        await asyncio.sleep(0.9)
        await ws.send(json.dumps({"t": "key", "k": "use", "down": False}))
        await asyncio.sleep(1.5)
        if any(j.get("t") == "riptide" for j in inbox):
            note("FAIL: riptide on dry ground")
            fails.append("riptide on dry ground")
        else:
            note("OK: no riptide on dry ground")

        await cmd("kill @e[type=!player]", "item replace entity @a weapon.mainhand with minecraft:air", "effect clear @a")
        await ws.send(json.dumps({"t": "clear"}))
        await asyncio.sleep(0.3)

    print("ALL PASSED" if not fails else f"FAILED: {fails}")


asyncio.run(main())
