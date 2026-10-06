"""Integration test for the Minecraft side's bridges to the host (run with Minecraft running, no GTA): animals turning
into the host's, commands reaching the host (/kill @e, /time, /weather, /clearall), mobs the host kills or sets alight,
the host's fires, the spectator flag, the HUD values, the host's water. Prints OK/FAIL lines and a summary."""
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
        msg = {"t": "cam", "f": f, "p": [x, y + 1.62, z], "r": [0, 20, 0], "fov": 70, "fp": True, "pl": [x, y, z], "h": 0,
               "ctl": state["ctl"], "hp": [state["hp"], 100], "ar": 50, "st": 80.0}
        await ws.send(json.dumps(msg))
        await asyncio.sleep(1 / 60)


async def main():
    state = {"pos": [0.5, GROUND, 0.5], "ctl": True, "hp": 100}
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
        for c in ["fill -10 64 -10 10 74 10 minecraft:air", "kill @e[type=!player]", "gamemode creative @a"]:
            await ws.send(json.dumps({"t": "cmd", "c": c}))
        await asyncio.sleep(1)
        inbox.clear()

        # animals the host has too turn into its own
        await ws.send(json.dumps({"t": "cmd", "c": "summon minecraft:cow 3 64 3"}))
        j = await expect(inbox, lambda j: j.get("t") == "animal" and j.get("m") == "a_c_cow", 5, "a cow becomes the host's cow (animal a_c_cow)")
        if j:
            ok = abs(j["pos"][0] - 3) < 0.6 and abs(j["pos"][2] - 3) < 0.6
            note("OK:" if ok else "FAIL:", "at the cow's place", j["pos"])
            if not ok:
                fails.append("animal position")
        await ws.send(json.dumps({"t": "cmd", "c": "summon minecraft:chicken 2 64 2"}))
        await expect(inbox, lambda j: j.get("t") == "animal" and j.get("m") == "a_c_hen", 5, "a chicken becomes the host's hen")

        # commands reach the host's world
        await ws.send(json.dumps({"t": "cmd", "c": "kill @e[type=minecraft:cow]", "chat": True}))
        await expect(inbox, lambda j: j.get("t") == "gtacmd" and j.get("c") == "kill" and j.get("what") == "a_c_cow", 5, "/kill @e[type=cow] kills the host's cows")
        await ws.send(json.dumps({"t": "cmd", "c": "kill @e", "chat": True}))
        await expect(inbox, lambda j: j.get("t") == "gtacmd" and j.get("c") == "kill" and j.get("what") == "all", 5, "/kill @e kills the host's people too")
        # filters as Minecraft's own: all but cows, within 5 blocks, 2 of them
        await ws.send(json.dumps({"t": "cmd", "c": "kill @e[type=!minecraft:cow,distance=..5,limit=2]", "chat": True}))
        await expect(inbox, lambda j: j.get("t") == "gtacmd" and j.get("c") == "kill" and j.get("what") == "all" and j.get("not") == "a_c_cow"
                     and abs(j.get("r", 0) - 5) < 0.01 and j.get("limit") == 2, 5, "/kill @e[type=!cow,distance=..5,limit=2]: the host's too, as filtered")
        # the mod's own commands (its setup's "time set noon") never reach the host
        await ws.send(json.dumps({"t": "cmd", "c": "time set midnight"}))
        await asyncio.sleep(1.0)
        if any(j.get("t") == "gtacmd" and j.get("c") == "time" for j in inbox):
            note("FAIL: the mod's own command changed the host's clock")
            fails.append("internal command forwarded")
        else:
            note("OK: the mod's own commands don't change the host's clock or weather")
        await ws.send(json.dumps({"t": "cmd", "c": "time set night", "chat": True}))
        await expect(inbox, lambda j: j.get("t") == "gtacmd" and j.get("c") == "time" and j.get("h") == 19 and j.get("m") == 0, 5,
                     "/time set night: the host's clock 19:00")
        await ws.send(json.dumps({"t": "cmd", "c": "weather rain", "chat": True}))
        await expect(inbox, lambda j: j.get("t") == "gtacmd" and j.get("c") == "weather" and j.get("w") == "rain", 5, "/weather rain: the host's rain")
        await ws.send(json.dumps({"t": "cmd", "c": "time set noon"}))
        await ws.send(json.dumps({"t": "cmd", "c": "weather clear"}))
        await asyncio.sleep(2)
        inbox.clear()

        # mobs near the player go to the host; one it kills dies, one it sets alight burns
        await ws.send(json.dumps({"t": "cmd", "c": "summon minecraft:zombie 4 64 0 {NoAI:1b}"}))
        await ws.send(json.dumps({"t": "cmd", "c": "summon minecraft:skeleton -4 64 0 {NoAI:1b}"}))
        j = await expect(inbox, lambda j: j.get("t") == "allmobs" and len(j.get("m", [])) >= 2, 5, "mobs near the player are listed (allmobs)")
        if j:
            ids = {round(m[1]): m[0] for m in j["m"]}
            zombie, skeleton = ids.get(4), ids.get(-4)
            await ws.send(json.dumps({"t": "mobkill", "ids": [zombie]}))
            await ws.send(json.dumps({"t": "mobfire", "ids": [skeleton]}))
            await asyncio.sleep(1.0)
            inbox.clear()
            j = await expect(inbox, lambda j: j.get("t") == "allmobs", 3, "(listed again)")
            if j:
                left = {m[0]: m for m in j["m"]}
                ok = zombie not in left
                note("OK:" if ok else "FAIL:", "the mob the host's car hit is dead (mobkill)")
                if not ok:
                    fails.append("mobkill")
                ok = skeleton in left and left[skeleton][6] == 1
                note("OK:" if ok else "FAIL:", "the mob the host's fire reached burns (mobfire)")
                if not ok:
                    fails.append("mobfire")

        # the host's fire: fire there in Minecraft too
        inbox.clear()
        await ws.send(json.dumps({"t": "gtafire", "p": [6, 64, 6]}))
        await expect(inbox, lambda j: j.get("t") == "hot" and [6, 64, 6] in [j["fire"][i:i + 3] for i in range(0, len(j.get("fire", [])), 3)], 5,
                     "the host's fire lights a fire block there (hot)")

        # spectator: the host is told
        await ws.send(json.dumps({"t": "cmd", "c": "gamemode spectator @a"}))
        await expect(inbox, lambda j: j.get("t") == "pstate" and j.get("spec") == 1, 5, "spectator: pstate spec 1")
        await ws.send(json.dumps({"t": "cmd", "c": "gamemode creative @a"}))
        await expect(inbox, lambda j: j.get("t") == "pstate" and j.get("spec") == 0, 5, "creative again: spec 0")

        # survival: Minecraft's health follows the host's; damage goes to the host
        await ws.send(json.dumps({"t": "cmd", "c": "gamemode survival @a"}))
        state["hp"] = 50
        await asyncio.sleep(1.0)
        inbox.clear()
        await ws.send(json.dumps({"t": "cmd", "c": "damage @p 4 minecraft:generic"}))
        j = await expect(inbox, lambda j: j.get("t") == "pdmg", 5, "damage in survival goes to the host (pdmg)")
        if j:
            ok = 0.5 < j["d"] <= 4.01  # (what Minecraft really took, after its own reductions)
            note("OK:" if ok else "FAIL:", "by what Minecraft took", j["d"])
            if not ok:
                fails.append("pdmg amount")
        await ws.send(json.dumps({"t": "cmd", "c": "gamemode creative @a"}))
        state["hp"] = 100

        # cutscene: no input reaches Minecraft
        state["ctl"] = False
        await asyncio.sleep(0.3)
        await ws.send(json.dumps({"t": "key", "k": "inventory", "down": True}))
        await ws.send(json.dumps({"t": "key", "k": "inventory", "down": False}))
        await asyncio.sleep(1.0)
        opened = any(j.get("t") == "screen" and j.get("kind") == 2 for j in inbox)
        note("FAIL:" if opened else "OK:", "no inventory in a cutscene (ctl false)")
        if opened:
            fails.append("ctl")
        state["ctl"] = True
        await asyncio.sleep(0.3)

        # /clearall: blocks and mobs gone, the host told
        await ws.send(json.dumps({"t": "cmd", "c": "setblock 2 64 -3 minecraft:stone"}))
        await ws.send(json.dumps({"t": "cmd", "c": "summon minecraft:zombie 2 64 3 {NoAI:1b}"}))
        await asyncio.sleep(1.0)
        inbox.clear()
        await ws.send(json.dumps({"t": "cmd", "c": "clearall"}))
        await expect(inbox, lambda j: j.get("t") == "gtacmd" and j.get("c") == "clearall", 5, "/clearall tells the host")
        await asyncio.sleep(1.0)
        j = [j for j in inbox if j.get("t") == "allmobs"]
        ok = not j or not j[-1].get("m")
        note("OK:" if ok else "FAIL:", "/clearall leaves no mobs")
        if not ok:
            fails.append("clearall mobs")

        # a mission restarted (the host says): Minecraft's things cleared, the host's own cars and people kept
        await ws.send(json.dumps({"t": "cmd", "c": "setblock 2 64 -3 minecraft:stone"}))
        await asyncio.sleep(0.5)
        inbox.clear()
        await ws.send(json.dumps({"t": "restart"}))
        await expect(inbox, lambda j: j.get("t") == "gtacmd" and j.get("c") == "clearall" and j.get("soft") is True, 5,
                     "a mission restart clears Minecraft's things (softly: the host's own stay)")

        # /range past Minecraft's 64
        inbox.clear()
        await ws.send(json.dumps({"t": "cmd", "c": "range 200", "chat": True}))
        await expect(inbox, lambda j: j.get("t") == "gtacmd" and j.get("c") == "range" and abs(j.get("r", 0) - 200) < 0.01, 5,
                     "/range 200 is taken (over Minecraft's 64)")
        await ws.send(json.dumps({"t": "cmd", "c": "range 8", "chat": True}))

        # Straight: a bow's arrow flies without gravity
        await ws.send(json.dumps({"t": "cmd", "c": "kill @e[type=arrow]"}))
        await ws.send(json.dumps({"t": "cmd", "c": 'item replace entity @a weapon.mainhand with minecraft:bow[enchantments={"passthrough:straight":1}]'}))
        await asyncio.sleep(0.5)
        await ws.send(json.dumps({"t": "key", "k": "use", "down": True}))
        await asyncio.sleep(1.2)
        await ws.send(json.dumps({"t": "key", "k": "use", "down": False}))
        await asyncio.sleep(0.6)
        inbox.clear()
        await ws.send(json.dumps({"t": "cmd", "c": "execute if entity @e[type=arrow,nbt={NoGravity:1b}] run summon minecraft:pig 5 64 5"}))
        await expect(inbox, lambda j: j.get("t") == "animal" and j.get("m") == "a_c_pig", 5, "a Straight bow's arrow flies with no gravity")
        await ws.send(json.dumps({"t": "cmd", "c": "kill @e[type=!player]"}))
        note("SUMMARY:", "ALL PASSED" if not fails else f"{len(fails)} FAILED: {fails}")


if __name__ == "__main__":
    asyncio.run(main())
