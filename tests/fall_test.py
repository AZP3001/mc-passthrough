"""Integration test for falls in Minecraft's movement (run with Minecraft running, no GTA): a real fall speeds up as
GTA's do (9.8 m/s per second, not Minecraft's 32), and it hurts (the host is told, "pdmg"); landing in water doesn't
hurt (a water-bucket clutch); and the void under the host's world doesn't kill the player. Prints OK/FAIL lines and a
summary."""
import asyncio
import json
import time

import websockets

URL = "ws://127.0.0.1:25599"
fails = []


def note(*a):
    print(f"[{time.strftime('%H:%M:%S')}] " + " ".join(str(x) for x in a), flush=True)


def check(ok, what):
    note("OK:" if ok else "FAIL:", what)
    if not ok:
        fails.append(what)


async def main():
    state = {"pos": [0.5, 64.0, 0.5], "walk": False}
    track = []
    inbox = []
    async with websockets.connect(URL, max_size=None) as ws:
        async def reader():
            async for m in ws:
                try:
                    j = json.loads(m)
                except Exception:
                    continue
                if j.get("t") == "mcpos":
                    if j.get("w") == 1:
                        track.append((time.time(), j))
                    continue
                inbox.append(j)

        async def cam_loop():
            f = 0
            while True:
                x, y, z = state["pos"]
                f += 1
                await ws.send(json.dumps({"t": "cam", "f": f, "p": [x, y + 1.62, z], "r": [0, 20, 0], "fov": 70, "fp": True,
                                          "pl": [x, y, z], "h": 0, "ctl": True, "hp": [100, 100], "ar": 0, "st": 100.0,
                                          "walk": state["walk"], "in": 0}))
                await asyncio.sleep(1 / 60)

        async def cmd(*cs):
            for c in cs:
                await ws.send(json.dumps({"t": "cmd", "c": c}))
            await asyncio.sleep(0.4)

        asyncio.create_task(reader())
        await ws.send(json.dumps({"t": "clear"}))
        await asyncio.sleep(0.3)
        # the host's ground (barriers up to y 62, its top at y 63) round x 0, z 0; its water (from its ground up to y 66) at x 20..23
        cols, water = [], []
        for x in range(-6, 26):
            for z in range(-6, 7):
                cols += [x, z, 59, 60] if 20 <= x <= 23 else [x, z, 61, 62]
                if 20 <= x <= 23:
                    water += [x, z, 61, 66 * 256]
        await ws.send(json.dumps({"t": "ground", "c": cols}))
        await ws.send(json.dumps({"t": "gwater", "c": water}))
        # the host's collision for the player Minecraft moves: its ground (top y 63, as 4-block tiles), and its sea
        # floor (top y 61) under the water
        hc = []
        for x in range(-8, 28, 4):
            for z in range(-8, 8, 4):
                hc += [x, 60 if 20 <= x < 24 else 62, z, x + 4, 61 if 20 <= x < 24 else 63, z + 4]
        await ws.send(json.dumps({"t": "hc", "b": hc}))
        cam = asyncio.create_task(cam_loop())
        await asyncio.sleep(2)
        await cmd("kill @e[type=!player]", "gamemode survival @a", "effect clear @a", "difficulty normal")
        state["walk"] = True
        await asyncio.sleep(1.5)
        pid = [100]

        async def drop(x, y, z):
            pid[0] += 1
            inbox[:] = [j for j in inbox if j.get("t") != "pdmg"]
            track.clear()
            await ws.send(json.dumps({"t": "pset", "id": pid[0], "pos": [x, y, z], "keepY": False}))

        # 1. a 40-block fall onto the host's ground: GTA's speed, and it hurts
        await drop(0.5, 103.0, 0.5)
        await asyncio.sleep(1.0)
        fall = [j["vel"][1] for t, j in track if j.get("ps") == pid[0]]
        speed = -min(fall, default=0.0)
        # (a second in: GTA's 9.8 m/s per second, Minecraft's own is about 26 by then)
        check(5.0 < speed < 17.0, f"a second into the fall: {speed:.1f} m/s (GTA's speed, not Minecraft's)")
        await asyncio.sleep(3.0)
        hurt = sum(j.get("d", 0) for j in inbox if j.get("t") == "pdmg")
        check(hurt >= 10.0, f"the fall hurt the host's player ({hurt:.1f} health)")
        last = track[-1][1]["pos"][1] if track else 0
        check(62.9 < last < 63.5, f"landed on the host's ground (y {last:.2f})")

        # 2. the same fall into the host's water: no hurt
        await asyncio.sleep(1.0)
        await drop(21.5, 103.0, 0.5)
        await asyncio.sleep(4.5)
        hurt = sum(j.get("d", 0) for j in inbox if j.get("t") == "pdmg")
        check(hurt < 0.5, f"a fall into water doesn't hurt ({hurt:.1f})")

        # 3. under the host's world (the void): no hurt, no death
        await drop(60.5, -150.0, 0.5)
        await asyncio.sleep(3.0)
        hurt = sum(j.get("d", 0) for j in inbox if j.get("t") == "pdmg")
        check(hurt < 0.5, f"the void doesn't hurt ({hurt:.1f})")
        check(not any(j.get("t") == "dead" or j.get("t") == "died" for j in inbox), "nor kill")
        # falling, then put on the host's ground (a respawn, the host's world loading in under the player): the fall
        # before doesn't count (it killed the player as it spawned)
        await drop(60.5, 140.0, 0.5)
        await asyncio.sleep(2.2)
        await drop(0.5, 63.2, 0.5)
        await asyncio.sleep(2.0)
        hurt = sum(j.get("d", 0) for j in inbox if j.get("t") == "pdmg")
        check(hurt < 0.5, f"falling, then put on the ground: no fall ({hurt:.1f})")

        # 4. creative (the mod's own world is creative): a fall hurts all the same, and a zombie hunts the player
        await cmd("gamemode creative @a")
        await asyncio.sleep(0.5)
        await drop(0.5, 83.0, 0.5)
        await asyncio.sleep(2.5)
        hurt = sum(j.get("d", 0) for j in inbox if j.get("t") == "pdmg")
        check(hurt >= 5.0, f"in creative a 20-block fall hurts too ({hurt:.1f})")
        inbox[:] = [j for j in inbox if j.get("t") != "pdmg"]
        await cmd("summon minecraft:zombie 4.5 63 0.5 {PersistenceRequired:1b}")
        await asyncio.sleep(6.0)
        hurt = sum(j.get("d", 0) for j in inbox if j.get("t") == "pdmg")
        check(hurt > 0.0, f"a zombie attacks the creative player ({hurt:.1f})")
        await cmd("kill @e[type=minecraft:zombie]")

        state["walk"] = False
        await asyncio.sleep(0.5)
        cam.cancel()
        note("SUMMARY:", "all passed" if not fails else f"{len(fails)} failed: {fails}")


asyncio.run(main())
