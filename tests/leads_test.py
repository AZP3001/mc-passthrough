"""Integration test for leads on the host's things and boats carrying its people (run with Minecraft running, no GTA):
the lead's ends (invisible allays) where the host says, held by Steve or tied; a lead used up from his hand; Minecraft's
own mobs tied at one of the host's spots; a right click with a lead reaching the host; boats reported, a seat for one of
the host's people, freed when the boat breaks. Prints OK/FAIL lines and a summary.

Each check runs a command that summons an animal only if the check holds: the mod turns it into one of the host's
("animal"), which is what the test waits for."""
import asyncio
import json
import time

import websockets

URL = "ws://127.0.0.1:25599"
GROUND = 64.0
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
        await ws.send(json.dumps({"t": "cam", "f": f, "p": [x, y + 1.62, z], "r": [0, 20, 0], "fov": 70, "fp": True, "pl": [x, y, z], "h": 0,
                                  "ctl": True, "hp": [100, 100], "ar": 0, "st": 100.0}))
        if state["leads"] is not None:
            await ws.send(json.dumps({"t": "leashes", "l": state["leads"]}))
        await asyncio.sleep(1 / 60)


async def main():
    state = {"pos": [0.5, GROUND, 0.5], "leads": None}
    async with websockets.connect(URL, max_size=None) as ws:
        inbox = []
        asyncio.create_task(reader(ws, inbox))
        cols = []
        for x in range(-20, 21):
            for z in range(-20, 21):
                cols += [x, z, 62, 63]
        await ws.send(json.dumps({"t": "ground", "c": cols}))
        asyncio.create_task(cam_loop(ws, state))
        await asyncio.sleep(3)

        async def cmd(*cs):
            for c in cs:
                await ws.send(json.dumps({"t": "cmd", "c": c}))
            await asyncio.sleep(0.4)

        async def check(condition, animal, model, what):
            # summons `animal` (turned into the host's `model`) only if the execute condition holds
            inbox[:] = [j for j in inbox if j.get("t") != "animal"]
            await cmd(f"execute {condition} run summon minecraft:{animal} 8 64 8")
            await expect(inbox, lambda j: j.get("t") == "animal" and j.get("m") == model, 3, what)

        await cmd("kill @e[type=!player]", "gamemode creative @a")
        await asyncio.sleep(1)
        inbox.clear()

        # a lead in Steve's hand on one of the host's things 3 blocks east: its end is an invisible allay there, leashed
        state["leads"] = [[1, 0, 3.5, 65.5, 0.5, 0.5, 65.1, 0.5]]
        await asyncio.sleep(1.5)
        await check("as @e[type=allay,tag=passthrough_proxy] at @s if entity @s[x=3,y=64.8,z=0,dx=1,dy=1,dz=1] if data entity @s leash",
                    "cow", "a_c_cow", "a lead in Steve's hand: its end where the host says, on a lead")
        await check("if entity @e[type=allay,tag=passthrough_proxy,nbt={NoAI:1b,Invulnerable:1b,Silent:1b}]", "chicken", "a_c_hen",
                    "the end is still, silent and can't be hurt")
        # it follows the host's thing
        state["leads"] = [[1, 0, -4.5, 65.5, 2.5, 0.5, 65.1, 0.5]]
        await asyncio.sleep(1.0)
        await check("as @e[type=allay,tag=passthrough_proxy] if entity @s[x=-5,y=64.8,z=2,dx=1,dy=1,dz=1]", "pig", "a_c_pig",
                    "the end moves with the host's thing")

        # tied to a spot (a wall): both ends there, the far one holding the lead
        state["leads"] = [[1, 1, -4.5, 65.5, 2.5, -4.5, 66.5, 6.5]]
        await asyncio.sleep(1.5)
        await check("as @e[type=allay,tag=passthrough_proxy] if entity @s[x=-5,y=65.5,z=6,dx=1,dy=1,dz=1] unless data entity @s leash",
                    "rabbit", "a_c_rabbit_01", "tied: the lead's other end at the spot, holding it")
        await check("as @e[type=allay,tag=passthrough_proxy] if entity @s[x=-5,y=64.8,z=2,dx=1,dy=1,dz=1] if data entity @s leash", "cat", "a_c_cat_01",
                    "and the thing's end is on that lead")

        # a new lead from Steve's hand (survival): used up
        await cmd("gamemode survival @a", "item replace entity @a weapon.mainhand with minecraft:lead 1")
        await ws.send(json.dumps({"t": "leashevt", "id": 1, "e": "new", "pos": [3.5, 65.5, 0.5]}))
        await asyncio.sleep(0.6)
        await check("unless items entity @a weapon.mainhand minecraft:lead", "fox", "a_c_coyote", "a new lead in survival uses one up")
        # cut: a lead drops there
        await ws.send(json.dumps({"t": "leashevt", "id": 1, "e": "cut", "pos": [2.5, 64.5, -2.5]}))
        await asyncio.sleep(0.6)
        await check("if entity @e[type=item,x=2,y=64,z=-3,dx=1,dy=1,dz=1]", "wolf", "a_c_husky", "cut: the lead drops where it was")
        await cmd("kill @e[type=item]", "gamemode creative @a")

        # Minecraft's own sheep on a lead in Steve's hand, tied at the host's spot: dropped when the spot goes
        await cmd("summon minecraft:sheep 1.5 64 -1.5", "execute as @e[type=sheep] run data modify entity @s leash.UUID set from entity @p UUID")
        await asyncio.sleep(1.0)
        state["leads"] = [[1, 1, -4.5, 65.5, 2.5, -4.5, 66.5, 6.5], [2, 2, 2.5, 65.0, -3.5, 2.5, 65.0, -3.5]]
        await asyncio.sleep(0.6)
        await ws.send(json.dumps({"t": "leashevt", "id": 2, "e": "tiemobs", "pos": [2.5, 65.0, -3.5]}))
        await asyncio.sleep(0.8)
        await cmd("kill @e[type=item]")
        state["leads"] = [[1, 1, -4.5, 65.5, 2.5, -4.5, 66.5, 6.5]]
        await asyncio.sleep(1.0)
        await check("if entity @e[type=item,nbt={Item:{id:\"minecraft:lead\"}}]", "ocelot", "a_c_mtlion",
                    "Minecraft's sheep was tied at the host's spot (its lead drops when the spot goes)")

        # the host's lead list empty: no ends left
        state["leads"] = []
        await asyncio.sleep(1.0)
        await check("unless entity @e[type=allay,tag=passthrough_proxy]", "salmon", "a_c_fish", "no leads: no ends")
        state["leads"] = None

        # a right click with a lead at the host's world: the host hears of it
        inbox.clear()
        await cmd("item replace entity @a weapon.mainhand with minecraft:lead 1")
        await asyncio.sleep(0.5)
        await ws.send(json.dumps({"t": "key", "k": "use", "down": True}))
        await asyncio.sleep(0.1)
        await ws.send(json.dumps({"t": "key", "k": "use", "down": False}))
        await expect(inbox, lambda j: j.get("t") == "leashuse" and j.get("item") == "lead", 3, "a right click with a lead reaches the host")
        await cmd("item replace entity @a weapon.mainhand with minecraft:shears 1")
        await asyncio.sleep(0.5)
        await ws.send(json.dumps({"t": "key", "k": "use", "down": True}))
        await asyncio.sleep(0.1)
        await ws.send(json.dumps({"t": "key", "k": "use", "down": False}))
        await expect(inbox, lambda j: j.get("t") == "leashuse" and j.get("item") == "shears", 3, "and with shears")

        # a boat: reported (two free seats, it takes people in); a seat for one of the host's people; broken: freed
        inbox.clear()
        await cmd("summon minecraft:oak_boat 3.5 64 -3.5")
        j = await expect(inbox, lambda j: j.get("t") == "boats" and any(r[8] == 2 and r[9] == 1 for r in j.get("b", [])), 3,
                         "a boat is reported to the host (2 free seats, takes people in)")
        if j:
            boat = j["b"][0][0]
            await ws.send(json.dumps({"t": "boatgrab", "boat": boat, "ped": 777}))
            s = await expect(inbox, lambda j: j.get("t") == "boats" and any(r[0] == 777 and r[1] == boat for r in j.get("s", [])), 3,
                             "one of the host's people gets a seat in it")
            if s:
                row = [r for r in s["s"] if r[0] == 777][0]
                ok = abs(row[2] - 3.5) < 1.2 and abs(row[4] + 3.5) < 1.2
                note("OK:" if ok else "FAIL:", "the seat is in the boat", row)
                if not ok:
                    fails.append("seat position")
            await expect(inbox, lambda j: j.get("t") == "boats" and any(r[0] == boat and r[8] == 1 for r in j.get("b", [])), 3,
                         "the boat has one seat left")
            await cmd("kill @e[type=oak_boat]")
            await expect(inbox, lambda j: j.get("t") == "boats" and not j.get("s"), 3, "the boat broken: the seat is gone")
            await asyncio.sleep(0.5)
            await check("unless entity @e[type=allay,tag=passthrough_proxy]", "dolphin", "a_c_dolphin", "and its proxy too")
        await cmd("kill @e[type=!player]")

    print("ALL PASSED" if not fails else f"FAILED: {fails}")


asyncio.run(main())
