"""Integration test for mining the host's world with a pickaxe (run with Minecraft running, no GTA): the crosshair on the
host's wall with a pickaxe puts the piece there; survival mining breaks it (cracks, a drop, the host told "dug"); not
with a sword; the host's cells round the hole filled with Minecraft's blocks, which mine on the same way; /clearall
puts it all back. Prints OK/FAIL lines and a summary.

Each check runs a command that summons an animal only if the check holds: the mod turns it into one of the host's
("animal"), which is what the test waits for. Saves a screenshot mid-mining (cracks on the unseen piece) if `import`
(ImageMagick) and DISPLAY are there."""
import asyncio
import json
import os
import subprocess
import time

import websockets

URL = "ws://127.0.0.1:25599"
fails = []


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
        msg = {"t": "cam", "f": f, "p": [x, y + 1.62, z], "r": [-90, 0, 0], "fov": 70, "fp": True, "pl": [x, y, z], "h": -90,
               "ctl": True, "hp": [100, 100], "ar": 0, "st": 100.0}
        if state["gh"] is not None:
            msg["gh"] = state["gh"]
            msg["ghOn"] = True
        await ws.send(json.dumps(msg))
        await asyncio.sleep(1 / 60)


async def main():
    # Steve at (0.5, 64, 0.5) looking east (+x); the host's wall 2.8 blocks ahead, its face at x 3.3 (facing west),
    # its material rock, the map's own
    wall = [3.3, 65.2, 0.5, -1.0, 0.0, 0.0, 0, 0]
    state = {"pos": [0.5, 64.0, 0.5], "gh": None}
    async with websockets.connect(URL, max_size=None) as ws:
        inbox = []
        asyncio.create_task(reader(ws, inbox))
        await ws.send(json.dumps({"t": "clear"}))
        await asyncio.sleep(0.3)
        cols = []
        for x in range(-12, 13):
            for z in range(-12, 13):
                cols += [x, z, 62, 63]
        await ws.send(json.dumps({"t": "ground", "c": cols}))
        asyncio.create_task(cam_loop(ws, state))
        await asyncio.sleep(3)

        async def cmd(*cs):
            for c in cs:
                await ws.send(json.dumps({"t": "cmd", "c": c}))
            await asyncio.sleep(0.4)

        async def check(condition, animal, model, what):
            inbox[:] = [j for j in inbox if j.get("t") != "animal"]
            await cmd(f"execute {condition} run summon minecraft:{animal} -8 64 -8")
            await expect(inbox, lambda j: j.get("t") == "animal" and j.get("m") == model, 3, what)

        async def attack(seconds):
            await ws.send(json.dumps({"t": "key", "k": "attack", "down": True}))
            await asyncio.sleep(seconds)
            await ws.send(json.dumps({"t": "key", "k": "attack", "down": False}))

        await cmd("kill @e[type=!player]", "gamemode survival @a", "fill 2 63 -2 6 67 2 minecraft:air",
                  "item replace entity @a weapon.mainhand with minecraft:iron_sword")
        await asyncio.sleep(1)
        inbox.clear()

        # a sword at the host's wall: nothing to mine there
        state["gh"] = wall
        await asyncio.sleep(1.0)
        await check("unless block 3 65 0 #minecraft:mineable/pickaxe", "cow", "a_c_cow", "a sword at the host's wall: no piece to mine")
        # a pickaxe: the piece where the wall is (the cell just behind its face)
        await cmd("item replace entity @a weapon.mainhand with minecraft:stone_pickaxe")
        await asyncio.sleep(1.0)
        await check("if block 3 65 0 passthrough:gta_rock[facing=west,depth=4]", "chicken", "a_c_hen",
                    "a pickaxe at the host's wall: the piece there (facing it, shaped to it)")
        # survival mining: takes a while (a stone pickaxe on rock), then it breaks, the host is told and it drops
        inbox.clear()
        await ws.send(json.dumps({"t": "key", "k": "attack", "down": True}))
        await asyncio.sleep(0.25)
        early = any(j.get("t") == "dug" for j in inbox)
        note("FAIL:" if early else "OK:", "not broken at once (survival mining takes time)")
        if early:
            fails.append("broken at once")
        if os.environ.get("DISPLAY") and subprocess.call(["which", "import"], stdout=subprocess.DEVNULL) == 0:
            # (in a thread: the camera keeps coming meanwhile)
            shot = os.path.join(os.environ.get("SHOTS", "."), "dig_cracks.png")
            await asyncio.to_thread(subprocess.call, ["import", "-window", "root", shot])
            note("screenshot:", shot)
        await expect(inbox, lambda j: j.get("t") == "dug" and j.get("c") == [3, 65, 0] and j.get("k") == 0, 5,
                     "mined: the host is told the cell is dug out (rock)")
        await ws.send(json.dumps({"t": "key", "k": "attack", "down": False}))
        await check("if entity @e[type=item,nbt={Item:{id:\"minecraft:cobblestone\"}}]", "pig", "a_c_pig", "and it drops cobblestone")
        await check("if items entity @p weapon.mainhand minecraft:stone_pickaxe[damage=1]", "rabbit", "a_c_rabbit_01", "the pickaxe wears")
        await check("if block 3 65 0 minecraft:air", "cat", "a_c_cat_01", "the cell is empty")

        # the host's cells round it still solid: Minecraft's blocks there (the hole's sides), and they mine on
        state["gh"] = None
        await ws.send(json.dumps({"t": "digfill", "c": [4, 65, 0, 0, 3, 64, 0, 1]}))
        await asyncio.sleep(0.6)
        await check("if block 4 65 0 minecraft:stone if block 3 64 0 minecraft:dirt", "fox", "a_c_coyote",
                    "the cells round the hole filled with Minecraft's blocks (stone in rock, dirt in soil)")
        inbox.clear()
        await cmd("item replace entity @a weapon.mainhand with minecraft:diamond_pickaxe")
        await asyncio.sleep(0.5)
        await attack(2.5)
        await expect(inbox, lambda j: j.get("t") == "dug" and j.get("c") == [4, 65, 0], 3, "a filling block mined: dug out too")

        # /clearall: all put back
        await cmd("clearall")
        await asyncio.sleep(1.0)
        await check("unless block 3 64 0 minecraft:dirt unless block 3 65 0 passthrough:gta_rock", "wolf", "a_c_husky",
                    "/clearall: the filling blocks and the piece gone")
        await cmd("kill @e[type=!player]", "gamemode creative @a", "item replace entity @a weapon.mainhand with minecraft:air")
        await ws.send(json.dumps({"t": "clear"}))
        await asyncio.sleep(0.3)

    print("ALL PASSED" if not fails else f"FAILED: {fails}")


asyncio.run(main())
