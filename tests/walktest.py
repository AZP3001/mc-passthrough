"""Fake GTA host for Minecraft's movement ("walk" mode): GTA collision boxes (a fractional floor, walls, a low wall, a
ceiling), keys, corrections; plus pearls, arrows through the host's ground blocks, blocks placed against host walls,
water reports and the worn elytra. Prints OK/FAIL lines and a summary."""
import asyncio
import json
import math
import time

import websockets

URL = "ws://127.0.0.1:25599"
FLOOR = 64.3
fails = []
S = {"walk": False, "in": 0, "pl": [0.5, FLOOR, 0.5], "yaw": 0.0, "pitch": 10.0, "mc": None, "gh": None, "dead": False,
     "fp": False}
track = []  # (time, x, y, z, ps) from mcpos while walking


def note(*a):
    print(f"[{time.strftime('%H:%M:%S')}] " + " ".join(str(x) for x in a), flush=True)


def check(ok, what):
    note("OK:" if ok else "FAIL:", what)
    if not ok:
        fails.append(what)


async def reader(ws, inbox):
    async for m in ws:
        try:
            j = json.loads(m)
        except Exception:
            continue
        t = j.get("t")
        if t == "mcpos":
            if j.get("w") == 1:
                S["mc"] = j
                track.append((time.time(), *j["pos"], j.get("ps"), j.get("g")))
            continue
        inbox.append(j)
        if t not in ("proj", "blocks", "screen"):
            note("<-", m[:160])


def boxes():
    b = []
    # the floor: 4 m tiles, top at 64.3 (between the host's ground blocks' whole-block tops)
    for x in range(-20, 20, 4):
        for z in range(-20, 20, 4):
            if 8 <= x < 12 and -4 <= z < 4:
                continue
            b += [x, FLOOR - 1.2, z, x + 4, FLOOR, z + 4]
    # a raised floor at 64.6, under a ground block whose top is 65
    b += [8, 63.4, -4, 12, 64.6, 4]
    # a tall wall facing -z at z = 6
    b += [-4, 63.3, 6.0, 4, 67.3, 6.3]
    # a low wall (1 m) facing +z at z = -6
    b += [-4, 63.3, -6.3, 4, FLOOR + 1.0, -6.0]
    # a ceiling 2.2 m over the floor
    b += [-16, FLOOR + 2.2, -3, -10, FLOOR + 3.2, 3]
    return b


async def cam_loop(ws):
    f = 0
    while True:
        f += 1
        yaw, pitch = S["yaw"], S["pitch"]
        yr, pr = math.radians(yaw), math.radians(pitch)
        fwd = [-math.sin(yr) * math.cos(pr), -math.sin(pr), math.cos(yr) * math.cos(pr)]
        if S["walk"] and S["mc"]:
            x, y, z = S["mc"]["pos"]
        else:
            x, y, z = S["pl"]
        if S["fp"]:
            cam = [x, y + 1.62, z]
        else:
            cam = [x - fwd[0] * 3.5, y + 1.9 - fwd[1] * 3.5, z - fwd[2] * 3.5]
        aim = [x + fwd[0] * 30, y + 1.6 + fwd[1] * 30, z + fwd[2] * 30]
        if S.get("aimOff"):
            aim = [x + S["aimOff"][0], y + 1.6, z + S["aimOff"][1]]
        msg = {"t": "cam", "f": f, "p": cam, "r": [yaw, pitch, 0], "fov": 70, "fp": S["fp"], "pl": S["pl"], "h": yaw, "gun": False,
               "veh": False, "aim": aim, "aimOn": not S["fp"], "walk": S["walk"], "in": S["in"], "dead": S["dead"]}
        if S["gh"]:
            msg["gh"] = S["gh"]
            msg["ghOn"] = True
        await ws.send(json.dumps(msg))
        if f % 3 == 0 and S["walk"]:
            await ws.send(json.dumps({"t": "hc", "b": boxes()}))
        await asyncio.sleep(1 / 60)


async def expect(inbox, pred, timeout, what):
    t0 = time.time()
    while time.time() - t0 < timeout:
        for j in list(inbox):
            if pred(j):
                inbox.remove(j)
                check(True, what)
                return j
        await asyncio.sleep(0.05)
    check(False, what)
    return None


PS = [0]


async def pset(ws, x, y, z):
    PS[0] += 1
    await ws.send(json.dumps({"t": "pset", "id": PS[0], "pos": [x, y, z], "keepY": False}))
    t0 = time.time()
    while time.time() - t0 < 3:
        if S["mc"] and S["mc"].get("ps") == PS[0]:
            return True
        await asyncio.sleep(0.02)
    return False


def since(t0):
    return [p for p in track if p[0] >= t0 and p[4] == PS[0]]


async def main():
    async with websockets.connect(URL, max_size=None) as ws:
        inbox = []
        asyncio.create_task(reader(ws, inbox))
        await ws.send(json.dumps({"t": "view", "w": 1280, "h": 720}))
        cols = []
        for x in range(-25, 26):
            for z in range(-25, 26):
                top = 64 if (8 <= x < 12 and -4 <= z < 4) else 63
                cols += [x, z, top - 1, top]
        await ws.send(json.dumps({"t": "ground", "c": cols}))
        asyncio.create_task(cam_loop(ws))
        await asyncio.sleep(3)
        for c in ["fill -20 64 -20 20 72 20 minecraft:air", "kill @e[type=!player]", "item replace entity @a armor.chest with minecraft:air"]:
            await ws.send(json.dumps({"t": "cmd", "c": c}))
        await asyncio.sleep(1)
        inbox.clear()

        # --- into Minecraft's movement: it starts where the host's player stands, on the host's (fractional) floor
        await ws.send(json.dumps({"t": "hc", "b": boxes()}))
        S["walk"] = True
        ok = await pset(ws, 0.5, FLOOR, 0.5)
        check(ok, "walk mode: Minecraft reports positions (w=1) with the host's correction id")
        await asyncio.sleep(1.0)
        y = S["mc"]["pos"][1] if S["mc"] else -1
        check(abs(y - FLOOR) < 0.03, f"stands on the host's floor at {FLOOR} (not the ground blocks' 64): y={y:.3f}")

        # --- walking: Minecraft's speed made 1.4 times faster (6.04 m/s), stopped by the host's wall at z=6 (half-width 0.3)
        S.update(yaw=0.0, pitch=10.0, **{"in": 1})
        t0 = time.time()
        await asyncio.sleep(2.5)
        S["in"] = 0
        pts = [p for p in since(t0) if p[0] - t0 > 0.4 and p[3] < 5.0]
        if len(pts) > 2:
            v = (pts[-1][3] - pts[0][3]) / (pts[-1][0] - pts[0][0])
            check(5.4 < v < 6.7, f"walks at Minecraft's speed x1.4: {v:.2f} m/s")
        else:
            check(False, "walk speed measured")
        z = S["mc"]["pos"][2]
        check(5.6 < z < 5.75, f"stops at the host's wall (z=6 - 0.3): z={z:.3f}")

        # --- W goes where the camera looks, even when Steve's head turns to a crosshair point off to the side
        await pset(ws, 0.5, FLOOR, -18.0)
        S.update(yaw=0.0, pitch=0.0, aimOff=[4.0, 4.0], **{"in": 1})
        await asyncio.sleep(1.0)
        S["in"] = 0
        await asyncio.sleep(0.3)
        x, z = S["mc"]["pos"][0], S["mc"]["pos"][2]
        check(abs(x - 0.5) < 0.15 and z > -15.5, f"walks along the camera, not the head turned to aim: dx={x - 0.5:.2f} dz={z + 18.0:.2f}")
        S["aimOff"] = None

        # --- sprinting (Ctrl): 5.6 m/s, toward -x
        await pset(ws, 6.5, FLOOR, 0.5)
        S.update(yaw=90.0, **{"in": 1 | 64})
        t0 = time.time()
        await asyncio.sleep(1.6)
        S["in"] = 0
        pts = [p for p in since(t0) if p[0] - t0 > 0.5]
        if len(pts) > 2:
            v = -(pts[-1][1] - pts[0][1]) / (pts[-1][0] - pts[0][0])
            check(7.0 < v < 8.8, f"sprints at Minecraft's speed x1.4: {v:.2f} m/s")
        else:
            check(False, "sprint speed measured")
        await asyncio.sleep(0.5)

        # --- jumping: 1.25 blocks
        await pset(ws, 0.5, FLOOR, -2.5)
        await asyncio.sleep(0.5)
        t0 = time.time()
        S["in"] = 16
        await asyncio.sleep(0.12)
        S["in"] = 0
        await asyncio.sleep(1.2)
        top = max((p[2] for p in since(t0)), default=0)
        check(abs(top - (FLOOR + 1.2522)) < 0.06, f"jumps Minecraft's 1.25 blocks: peak {top - FLOOR:.3f}")
        y = S["mc"]["pos"][1]
        check(abs(y - FLOOR) < 0.03, f"lands back on the floor: y={y:.3f}")

        # --- a ceiling 2.2 m up stops the jump (head at 1.8 m)
        await pset(ws, -12.5, FLOOR, 0.5)
        await asyncio.sleep(0.5)
        t0 = time.time()
        S["in"] = 16
        await asyncio.sleep(0.12)
        S["in"] = 0
        await asyncio.sleep(1.0)
        top = max((p[2] for p in since(t0)), default=0)
        check(top - FLOOR < 0.45, f"the host's ceiling stops the jump: peak {top - FLOOR:.3f}")

        # --- a floor at 64.6 under a ground block topped at 65: stands on the host's floor
        await pset(ws, 10.5, 64.6, 0.5)
        await asyncio.sleep(1.0)
        y = S["mc"]["pos"][1]
        check(abs(y - 64.6) < 0.03, f"stands on the raised host floor 64.6 inside a ground block: y={y:.3f}")
        # walking off it onto the 64.3 floor and back up (a 0.3 step: Minecraft steps up)
        S.update(yaw=-90.0, **{"in": 1})  # yaw -90: facing +x
        await asyncio.sleep(1.2)
        S["in"] = 0
        x, y = S["mc"]["pos"][0], S["mc"]["pos"][1]
        check(x > 12.2 and abs(y - FLOOR) < 0.03, f"walks off the raised floor down to 64.3: x={x:.2f} y={y:.3f}")
        await asyncio.sleep(0.5)
        S.update(yaw=90.0, **{"in": 1})
        await asyncio.sleep(1.0)
        S["in"] = 0
        await asyncio.sleep(0.4)
        x, y = S["mc"]["pos"][0], S["mc"]["pos"][1]
        check(x < 12.0 and abs(y - 64.6) < 0.03, f"steps back up 0.3: x={x:.2f} y={y:.3f}")

        # --- the low wall: jump on it while walking into it
        await pset(ws, 0.5, FLOOR, -4.5)
        S.update(yaw=180.0, **{"in": 1})
        await asyncio.sleep(0.6)
        S["in"] = 1 | 16
        await asyncio.sleep(0.25)
        S["in"] = 1
        await asyncio.sleep(0.3)
        S["in"] = 0
        await asyncio.sleep(0.6)
        x, y, z = S["mc"]["pos"]
        check(abs(y - (FLOOR + 1.0)) < 0.05 or z < -6.3, f"jumps onto the 1 m wall: y={y:.3f} z={z:.3f}")

        # --- a Minecraft ladder: climbs it
        for c in ["fill 3 64 3 3 69 3 minecraft:stone", "fill 3 64 2 3 69 2 minecraft:ladder[facing=north]"]:
            await ws.send(json.dumps({"t": "cmd", "c": c}))
        await asyncio.sleep(0.8)
        await pset(ws, 3.5, FLOOR, 1.3)
        S.update(yaw=0.0, pitch=0.0, **{"in": 1})
        await asyncio.sleep(2.0)
        S["in"] = 0
        y = S["mc"]["pos"][1]
        check(y > FLOOR + 1.0, f"climbs a Minecraft ladder: y={y:.2f}")
        await asyncio.sleep(0.5)

        # --- an arrow (looking down) flies through the host's ground blocks (the host traces it)
        await pset(ws, -4.5, FLOOR, -1.5)
        S.update(yaw=0.0, pitch=50.0)
        await asyncio.sleep(0.5)
        inbox.clear()
        await ws.send(json.dumps({"t": "slot", "n": 3}))
        await asyncio.sleep(0.3)
        await ws.send(json.dumps({"t": "key", "k": "use", "down": True}))
        await asyncio.sleep(1.2)
        await ws.send(json.dumps({"t": "key", "k": "use", "down": False}))
        await asyncio.sleep(1.5)
        ys = [p[3] for j in inbox if j.get("t") == "proj" for p in j["p"] if p[1].split(":")[0] == "arrow"]
        check(bool(ys) and min(ys) < 61.0, f"an arrow passes the host's ground blocks: lowest y {min(ys) if ys else None}")

        # --- an ender pearl is traced by the host; its hit teleports Steve
        S.update(pitch=-10.0)
        await asyncio.sleep(0.3)
        inbox.clear()
        await ws.send(json.dumps({"t": "slot", "n": 0}))
        await asyncio.sleep(0.3)
        await ws.send(json.dumps({"t": "key", "k": "use", "down": True}))
        await asyncio.sleep(0.1)
        await ws.send(json.dumps({"t": "key", "k": "use", "down": False}))
        j = await expect(inbox, lambda j: j.get("t") == "proj" and any(p[1] == "pearl" for p in j["p"]), 4, "an ender pearl is reported to the host")
        if j:
            pid = [p for p in j["p"] if p[1] == "pearl"][0][0]
            await ws.send(json.dumps({"t": "projhit", "id": pid, "pos": [-4.5, FLOOR + 0.05, 2.5], "stick": False}))
            j2 = await expect(inbox, lambda j: j.get("t") == "pteleport", 4, "the host's hit lands the pearl: Steve teleports there")
            if j2:
                p = j2["pos"]
                check(abs(p[0] + 4.5) < 0.2 and abs(p[2] - 2.5) < 0.2, f"teleported to the host's hit point: {p}")

        # --- a block placed against the host's wall (crosshair on it: z=6 face, normal -z)
        await pset(ws, 0.5, FLOOR, 3.0)
        S.update(yaw=0.0, pitch=-5.0, gh=[0.5, 65.5, 6.0, 0.0, 0.0, -1.0])
        await ws.send(json.dumps({"t": "slot", "n": 7}))
        await asyncio.sleep(0.6)
        inbox.clear()
        await ws.send(json.dumps({"t": "key", "k": "use", "down": True}))
        await asyncio.sleep(0.15)
        await ws.send(json.dumps({"t": "key", "k": "use", "down": False}))
        await expect(inbox, lambda j: j.get("t") == "blocks" and "0,65,5" in ",".join(map(str, j.get("set", []))), 4,
                     "a block placed against the host's wall (cell 0,65,5)")
        # and against the host's ceiling (normal -y): the cell under it
        S.update(gh=[-12.5, FLOOR + 2.2, 0.5, 0.0, -1.0, 0.0])
        await pset(ws, -12.5, FLOOR, -1.5)
        S.update(yaw=0.0, pitch=-60.0)
        await asyncio.sleep(0.5)
        await ws.send(json.dumps({"t": "key", "k": "use", "down": True}))
        await asyncio.sleep(0.15)
        await ws.send(json.dumps({"t": "key", "k": "use", "down": False}))
        await expect(inbox, lambda j: j.get("t") == "blocks" and "-13,66,0" in ",".join(map(str, j.get("set", []))), 4,
                     "a block placed under the host's ceiling (cell -13,66,0)")
        S["gh"] = None

        # --- water: reported with its flow
        await ws.send(json.dumps({"t": "cmd", "c": "setblock 2 64 -2 minecraft:water"}))
        j = await expect(inbox, lambda j: j.get("t") == "water" and "2,64,-2" in ",".join(map(str, j.get("set", []))), 4, "water reported to the host")
        await asyncio.sleep(2.0)
        flows = [j for j in inbox if j.get("t") == "water"]
        check(len(flows) > 0, f"spreading water reported ({len(flows)} messages)")
        await ws.send(json.dumps({"t": "cmd", "c": "fill -1 63 -5 5 66 1 minecraft:air"}))
        await expect(inbox, lambda j: j.get("t") == "water" and len(j.get("clear", [])) > 0, 4, "water gone reported")

        # --- the elytra worn is told to the host
        await ws.send(json.dumps({"t": "cmd", "c": "item replace entity @a armor.chest with minecraft:elytra"}))
        await expect(inbox, lambda j: j.get("t") == "pstate" and j.get("ely") == 1, 4, "wearing an elytra is told to the host")
        await ws.send(json.dumps({"t": "glide", "on": True, "speed": 1.0, "equip": False}))
        await asyncio.sleep(0.3)
        await ws.send(json.dumps({"t": "cmd", "c": "item replace entity @a armor.chest with minecraft:air"}))
        await expect(inbox, lambda j: j.get("t") == "pstate" and j.get("ely") == 0, 4, "taking it off is told too")

        # --- dead pose and the HUD hidden (no crash), then back to the host moving the player
        S["dead"] = True
        await ws.send(json.dumps({"t": "hud", "hidden": True}))
        await asyncio.sleep(1.0)
        S["dead"] = False
        await ws.send(json.dumps({"t": "hud", "hidden": False}))
        S["walk"] = False
        S["pl"] = [1.5, FLOOR, 1.5]
        await asyncio.sleep(1.0)
        await ws.send(json.dumps({"t": "cmd", "c": "tp @a 1.5 64.3 1.5"}))
        await asyncio.sleep(0.5)
        note("SUMMARY:", "all passed" if not fails else f"{len(fails)} failed: {fails}")


if __name__ == "__main__":
    asyncio.run(main())
