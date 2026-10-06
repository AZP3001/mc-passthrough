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
        await ws.send(json.dumps({"t": "cam", "f": f, "p": [x, y + 1.62, z], "r": [state.get("yaw", 0), state.get("pitch", 20), 0], "fov": 70, "fp": True, "pl": [x, y, z], "h": 0,
                                  "ctl": True, "hp": [100, 100], "ar": 0, "st": 100.0, "walk": state["walk"], "in": 0,
                                  "air": state.get("air", -1.0)}))
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
                elif -15 <= x <= -12 and -3 <= z <= 3:
                    water += [x, z, 64, int(64.3 * 256)]  # a foot-deep puddle of the host's water on its ground
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
        # the host hands its player over to Minecraft for the flight (as GTA does on "riptide"): it flies off, its launch
        # not lost in the handover (Minecraft's player was put back at the host's every frame, and stopped when walking began)
        state["walk"] = True
        await asyncio.sleep(0.6)
        moved = [j for j in inbox if j.get("t") == "mcpos"]
        far = max((abs(j["pos"][0] - 3.5) + abs(j["pos"][1] - 60.0) + abs(j["pos"][2] - 0.5) for j in moved), default=0.0)
        note("OK:" if far > 4.0 else "FAIL:", f"the riptide flies the player off ({far:.1f} blocks)")
        if far <= 4.0:
            fails.append("riptide flight")
        state["walk"] = False
        state["pos"] = [3.5, 60.0, 0.5]
        await asyncio.sleep(1.0)
        # the host's breath under its water (while it moves the player) is Minecraft's air
        state["pos"] = [3.5, 60.0, 0.5]
        state["air"] = 0.5
        await asyncio.sleep(1.0)
        await check("as @p if entity @s[nbt={Air:150s}]", "fox", "a_c_coyote", "the host's half breath: Minecraft's air half (150)")
        await asyncio.sleep(2.5)
        await check("as @p if entity @s[nbt={Air:150s}]", "wolf", "a_c_husky", "and Minecraft doesn't run it down itself")
        state["air"] = -1.0

        # a fishing rod cast into the host's water: the bobber floats in it (Minecraft fishes there)
        state["pos"] = [-1.5, 64.0, 0.5]
        state["yaw"], state["pitch"] = -90, -10  # (east, out over the water)
        await cmd('item replace entity @a weapon.mainhand with minecraft:fishing_rod')
        await asyncio.sleep(0.5)
        await ws.send(json.dumps({"t": "key", "k": "use", "down": True}))
        await asyncio.sleep(0.1)
        await ws.send(json.dumps({"t": "key", "k": "use", "down": False}))
        await asyncio.sleep(3.0)
        await check("if entity @e[type=fishing_bobber,x=0,y=58,z=-4,dx=8,dy=9,dz=8]", "pig", "a_c_pig",
                    "a bobber cast into the host's water floats in it")
        await cmd("kill @e[type=fishing_bobber]")
        state["yaw"], state["pitch"] = 0, 20
        await cmd('item replace entity @a weapon.mainhand with minecraft:trident[enchantments={"minecraft:riptide":3}]')

        # experience where one of the host's people the player killed fell
        await cmd("kill @e[type=experience_orb]")
        await ws.send(json.dumps({"t": "xp", "pos": [-6.5, 64.0, -6.5], "n": 5}))
        await asyncio.sleep(0.6)
        await check("if entity @e[type=experience_orb]", "rabbit", "a_c_rabbit_01", "a kill in the host's world drops experience")
        await ws.send(json.dumps({"t": "hitmark", "kill": True, "shot": False}))
        await asyncio.sleep(0.3)

        # foot-deep host water: in water, and a riptide launches from it
        state["pos"] = [-13.5, 64.0, 0.5]
        await asyncio.sleep(1.0)
        await check(f"as @p if predicate {IN_WATER}", "cow", "a_c_cow", "foot-deep host water: in water in Minecraft")
        inbox.clear()
        await ws.send(json.dumps({"t": "key", "k": "use", "down": True}))
        await asyncio.sleep(0.9)
        await ws.send(json.dumps({"t": "key", "k": "use", "down": False}))
        await expect(inbox, lambda j: j.get("t") == "riptide", 3, "a riptide trident launches in foot-deep host water")
        await asyncio.sleep(1.5)

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
