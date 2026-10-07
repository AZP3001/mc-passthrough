# Minecraft × GTA V passthrough

Real Minecraft Java 26.3 runs next to GTA V (story mode) and is drawn into GTA's picture. GTA's camera drives
Minecraft's, GTA's ground and walls become collision in Minecraft, and Minecraft's picture (colour and depth, plus the
hand and HUD) is composited into GTA's frame against GTA's depth buffer. What happens in one game happens in the other.

You play GTA's story as Steve, with Minecraft's items, blocks, mobs and movement.

> Based on the `minecraft-gta5-passthrough` example from
> [rehan-remade/universal-modder](https://github.com/rehan-remade/universal-modder), with many additions.

---

## What you can do

**Steve in GTA**
- Steve replaces GTA's character everywhere: on foot, in cars, in cutscenes, when you die. It also works after a
  switch to Michael, Franklin or Trevor.
- While GTA moves the character, Steve copies its skeleton: his head, body, arms and legs follow GTA's bones. He sits
  in the car seat, climbs, aims and lies down the way the character does.
- Minecraft's look is matched to GTA's: lit by GTA's surfaces around it, with GTA's gamma, saturation and colour
  grade, and solid (not see-through or glowing).

**Minecraft movement** (F6 turns it on and off)
- On foot, Minecraft moves you, exactly as in Minecraft but 40% faster. That covers walking, Shift to sprint, C to
  sneak, its jump, swimming (in GTA's water too), ladders, slime, elytra, riding and knockback.
- Falls are GTA's: you speed up as GTA's characters do (9.8 m/s per second, up to about 52 m/s) and a long fall hurts
  (or kills) as Minecraft's fall damage, so you need a parachute or a water bucket. With a parachute on, a long fall
  hands over to GTA's skydive (F opens it).
- Falling through GTA's world, you're put back on its ground; Minecraft's void never kills you.
- Q takes cover, GTA's way. Unlocked doors open as you walk through them.
- GTA's floors, walls and ceilings are your collision.
- Free look: the mouse looks all the way up and down, so you can bridge and pillar up. V switches first and third
  person.
- Hold Space at a ledge that's too high to jump and GTA climbs it. F at a ladder climbs it.
- In a car, press Space twice and the car flies with you. Press Space twice again to drive.
- The elytra is 4× faster and accelerates 4× harder, and it can't fly through buildings.

**Building and items**
- Your blocks become solid in GTA: people, cars and bullets stop at them. Cars that crash into them dent 3× as much.
- You can place blocks and pour water on GTA's streets, walls and ceilings. Reach is 8 blocks.
- Water puts out GTA's fires and carries people and cars along.
- GTA's water (sea, lakes, pools) is Minecraft water too: you swim in it, mobs swim, boats float, and riptide works.
- Minecraft's fire and lava set fire to everything by them: people (alive or not), cars and things. Minecraft's fire
  also burns as GTA's own fire, so it spreads to grass, people and cars the way GTA's does.

**Weapons**
- Swords give Minecraft's knockback (20% stronger) without knocking people down; the Knockback enchantment sends them
  flying (they're hurt once they're on their way, so a killing blow still throws them).
- Punching a sign, bin or lamp post (or shooting it with a Punch bow) knocks it loose and flies it.
- Axes, maces, tridents, spears and fists all hit GTA's people and cars. Hitting a car dents it where you hit:
  a fist a little, an axe a lot.
- Enchantments work in GTA: Sharpness (more damage), Knockback, Fire Aspect (sets on fire), Looting (people drop more
  money), and Power, Punch and Flame on bows.
- Straight (the mod's own bow enchantment, one level, from the enchanting table or `/enchant`): the arrow flies dead
  straight with no gravity, and is gone after 500 blocks.
- A drawn bow or crossbow pointed at someone: they put their hands up (or run). Cops, soldiers and anyone fighting only
  look at you. Pointed at a car, its driver reacts as to a gun in GTA: most honk and drive off angry, some back off
  and flee, a few give up (everyone gets out, hands up), and now and then one drives at you.
- Arrows hit people and cars, burst the tyre they hit, and break GTA's glass (shop windows, car windows).
- Ender pearls fly 2.5× as far and land on GTA's walls, floors and ceilings. Thrown from a car, the car comes with you.
- TNT and creepers blow up in both games. Fireworks become rockets, and you can throw potions, wind charges and
  snowballs.

**Leads and boats**
- Right-click a GTA person, car, sign, door or anything loose with a lead and it's on the lead in your hand. Minecraft
  draws the lead.
- What you can pull depends on its weight. Slim people walk after you; pulled hard, people are dragged along the
  ground on their back, and bodies, signs, bins and small doors are dragged too. Heavy-set people, lamp posts and cars don't move, and the lead holds you back instead. A Strength
  potion lets you pull more.
- Holding leads, right-click something else to tie them there: another person, a car, a door, a wall, the street, a
  ceiling, or one of your blocks. Right-click the thing you're leading again to let go of it.
- When no lead is in your hand, right-click a tied thing (or the spot it's tied to) with a lead to take its lead back.
  Shears cut every lead on what you click.
- Tied things can't get further than the lead: a person tied to a lamp post stays there, and a car tied to a wall
  can't drive off. A car tied to a person drags them, and a car tied to another car tows it. Two of GTA's people or
  animals tied together (a dog and a person) pull each other by weight: the light one gives way most.
- Leads never snap. After a teleport, what's on the lead in your hand comes along beside you.
- Minecraft's own mobs on leads in your hand get tied to the GTA thing you click, too.
- Minecraft's boats take in GTA's people and animals who walk into them, exactly as they take in Minecraft's mobs:
  only while nobody rows them, up to two per boat (one in a chest boat). They sit in the boat wherever it goes, until
  the boat breaks.

**Mobs and the world**
- Spawn a cow, pig, chicken, rabbit, cat, wolf, fox, ocelot, dolphin or fish and it becomes GTA's own animal.
- Minecraft's mobs bump into GTA's walls. A car or a flung person that hits a mob kills it.
- Fire passes between the games: Minecraft fire burns GTA's people and cars, and GTA's fires burn Minecraft's mobs.
- Hostile mobs hunt GTA's people, and the police shoot back. They hunt you too, as in survival, even in creative:
  zombies hit, fire burns, water drowns, falls hurt, all on GTA's health. GTA's people drown in Minecraft's water.
- Nether and End portals turn the area around them into the Nether or the End. Walk back in to leave.

**HUD**
- Minecraft's hearts, armour and hunger bars show GTA's health, armour and stamina, in creative too.
- GTA's minimap stays visible. Minecraft only hides behind it while it's actually shown, not in cutscenes.
- The off-hand item is hidden in first person.

**Story missions stay playable**
- In cutscenes, character switches, loading screens and mission scenes, Minecraft gets no input, so you can't hit
  anyone by accident.
- Restarting a mission or going back to a checkpoint clears Minecraft's things (as `/clearall`), keeping the mission's
  own cars and people.
- GTA's slow motion (dying, Michael's and Franklin's abilities) slows Minecraft too: swings, the bow, the attack bar.
- GTA's phone shows over Minecraft's hand and HUD, and the clicks and wheel go to the phone while it's out.
- God mode (F9) is off by default, so the story runs by GTA's rules.

---

## Install (Windows)

You need everything in [Requirements](#requirements) first: Minecraft 26.3 with Fabric and Fabric API in Prism
Launcher, GTA V **Legacy** on Steam, WSL (Ubuntu) and Visual Studio's C++ tools.

### 1. Get the code

Open **PowerShell** and paste:

```powershell
git clone https://github.com/AZP3001/mc-passthrough C:\dev\mc-passthrough
```

### 2. Minecraft part

1. In Prism Launcher, right-click your Fabric 26.3 instance and pick **Folder**. Open **mods**.
2. Copy `C:\dev\mc-passthrough\dist\passthrough-0.1.0.jar` into **mods**. Fabric API must be in there too.
3. Give this instance its own game folder (not your normal worlds): the mod creates a void world called `passthrough`
   and changes some options.

### 3. GTA part

**Close GTA first.** Open **Ubuntu** (WSL) and paste these lines one at a time:

```bash
cd /mnt/c/dev/mc-passthrough
chmod +x gta/*.sh gradle.sh
./gta/fetch_deps.sh
./gta/build.sh
FORCE=1 ./gta/install.sh
```

`fetch_deps.sh` downloads ScriptHookV and ReShade (only needed once). `build.sh` builds `MCPassthrough.asi` with Visual
Studio. `install.sh` copies everything into your GTA V folder.

### 4. Play

1. Start the Minecraft instance in Prism. Leave its window open: it renders slowly when minimized.
2. Start GTA V from Steam. Click **Story Mode** yourself.
3. You'll see "Minecraft passthrough connected" in GTA.

In GTA's settings: windowed or borderless, "Pause game on focus loss" off, and depth of field off.

### Updating to a new version

PowerShell:

```powershell
cd C:\dev\mc-passthrough
git pull
```

Then copy the new `dist\passthrough-0.1.0.jar` into Prism's **mods** folder (replace the old one), close GTA, and in
Ubuntu:

```bash
cd /mnt/c/dev/mc-passthrough
./gta/build.sh
FORCE=1 ./gta/install.sh
```

---

## Controls

| key | what it does |
|---|---|
| W A S D | walk (Minecraft's movement) |
| Shift | sprint |
| C | sneak; flying: go down |
| Space | Minecraft's jump; flying: go up. **Hold** it at a ledge too high to jump and GTA climbs it |
| Space twice | in the air with an elytra on: glide. In a car (driver's seat, after `/fly`): the car flies with you; twice again to drive |
| mouse | look all the way up and down (Minecraft movement) |
| hold Ctrl | zoom in, like OptiFine's zoom; the mouse wheel zooms closer or further (Minecraft movement) |
| V | first person / third person (near, middle, far) |
| left mouse | attack: break blocks, swing what you hold |
| right mouse | use: place blocks, pour water, shoot, throw pearls and potions, eat |
| mouse wheel, 1-9 | hotbar |
| Q | GTA's cover (Minecraft's drop is off) |
| E or I | Minecraft's inventory (E goes to GTA when GTA asks for E) |
| Esc | closes Minecraft's screen (doesn't open GTA's pause menu); with nothing open: GTA's pause menu |
| hold Alt | over the inventory or chat: hides it, and you can move and look about; let go and it's back |
| T | Minecraft's chat and commands |
| Tab | Steve's hands: Minecraft items, or GTA's own weapons (for missions) |
| F | get in a car, or climb a ladder you stand at |
| F6 | Minecraft movement on / off (off: GTA's own walking) |
| F7 | the whole passthrough on / off |
| F8 | fix the ground (re-level), and re-read `MCPassthrough.ini` |
| F9 | god mode on / off (can't die, no police) |

Everything else (driving, the phone, the map) is GTA's own.

## Commands

Type them in Minecraft's chat (T):

| command | what it does |
|---|---|
| `/spectator` | spectator, as in Minecraft: fly through everything, unseen. Space up, C down |
| `/creative` | back to creative |
| `/clearall` | clears all mobs, items and blocks you placed, and puts back GTA's things you moved (signs, bins, doors knocked loose). GTA's cars and people stay. Your inventory stays |
| `/kill @e` | kills Minecraft's mobs **and** GTA's people and animals around you (`/kill @e[type=cow]`: only cows) |
| `/time set night` | sets GTA's clock too (`day`, `noon`, `night`, `midnight`, or a number) |
| `/weather rain` | sets GTA's weather too (`clear`, `rain`, `thunder`) |
| `/fly` | flying on / off (Space up, C down; a car flies with Space twice). Off: no flying at all |
| `/waypoint` | teleports you (and your car) to the waypoint set on GTA's map |
| `/range 12` | your reach in blocks, for blocks, mobs and GTA's people, cars and things (default 8, up to 512) |
| `/enchant @p knockback 1000` | any level, past Minecraft's limit (an item holds 255; higher levels count in GTA) |
| `/summon car` | GTA things: `car`, `truck`, `tank`, `plane`, `jet`, `heli`, `boat`, `bike`, `bus`, `police`, `npc`, `cop`, `soldier`, `animal` (a random one), any of GTA's animals (`a_c_deer`, `a_c_sharktiger`, ... all listed as you type), or any GTA model name (`adder`, `a_m_y_hipster_01`). Minecraft's own mobs summon as always |
| `/gta spawn car 5` | the same as `/summon`, and more of them at once; `/gta spawn adder drive` puts you at the wheel |
| `/gta superjump` | GTA's cheats and abilities, each on / off: `superjump`, `fastrun`, `fastswim`, `explosiveammo`, `fireammo`, `explosivemelee`, `slidey` (slidey cars), `moon` (low gravity), `slowmo`, `infiniteammo`, `neverwanted`, `onehit`, `drunk`, `blackout`, `freezetime`; and `wanted 0-5`, `heal`, `armor`, `weapons`, `traffic 0-3`, `crowds 0-3` (how many cars and people GTA has about), `clear` (none around you), `flip` / `fix` / `boost` (your car), `sethome` / `home`, `tp x y [z]`, `skyfall`, `ragdoll`. `/gta` alone lists them |
| `/tune max` | the car you sit in: every performance part maxed and turbo. Also `stock`, `engine 1-4`, `brakes`, `transmission`, `suspension`, `armor`, `turbo on/off`, `wheels N`, `spoiler`, `bumpers`, `visuals`, `xenon`, `tint 0-6`, `repair` |
| `/kit` | the starter kit again (you get it once; the game no longer clears your inventory every start) |
| `/netherportal` | a lit Nether portal a few blocks ahead |
| `/endportal` | an End portal a few blocks ahead |

Also:

- Steve is as tall as the GTA character he stands in for, and in first person you see his body, arms and legs.
- Minecraft's potions and effects reach GTA: speed, slowness, jump boost, strength, fire resistance, nausea; healing heals
  GTA's player. Thrown potions hit GTA's people and cars (harming eats a car, healing repairs it, swiftness shoves it...).
- Minecraft armour you wear protects GTA's player too (4% less damage a point).
- Hunger bar = stamina: sprinting and hard swimming use it up; empty, you can't sprint until it's back to a third.
- Arrows stick in people, cars and signs and move with them; arrows and fists break glass where they hit, like a bullet.
- Hits on GTA's people and cars show a hit marker with Minecraft's hit sounds (red, with the XP chime, for a kill), and
  people you kill drop experience orbs.
- Fishing rods work in GTA's sea, lakes and rivers; things thrown or shot into GTA's water splash; Impaling tridents
  hit harder in water and rain. While GTA moves you, its breath is your air bubbles.
- In a mission's armed vehicle (a Buzzard, a tank) the mouse fires its weapons; Q always takes cover.
- A swing hits what your crosshair is on (a person, a car, a thing), not everyone round about (the mace's falling smash
  still hits all round). Weapons knock along where you look: look up while hitting and they fly up. Lamp posts, signs,
  bins and car parts can be hit and launched too, much further with Knockback. (GTA's own map objects are never moved
  themselves, that crashed GTA: the plugin's copy takes the place of one and flies, and the original is back once
  you're far away or after `/clearall`.)
- Hold **left** Alt over the inventory to look about; AltGr (typing @ in chat) and the chat itself are left alone.
- Lava melts cars (tyres pop, the body caves in, it burns and is pushed back out) and street furniture; fire under a car
  sets it alight. Cars by your blocks don't despawn.
- Endermen from the End steal GTA's cars and drive off in them. No ghasts come out of the Nether portal.
- A bow drawn (or a loaded crossbow) at people: close up they put their hands up, further off they run.
- GTA's guns break Minecraft's blocks: glass and leaves at once, stone after a few shots, obsidian practically never
  (cracks show as they take damage); rockets and grenades blow them up.
- A shield held up blocks GTA's bullets and blows. A totem of undying in hand saves you from dying in GTA (and is used
  up, with Minecraft's animation). Golden apples' absorption adds GTA body armour. Eating refills stamina and some
  health. Night vision is GTA's night vision; invisible, GTA's people and police don't see you. A spyglass zooms GTA's
  camera (Minecraft movement).
- Compasses point to your GTA map waypoint. Hit a parked car and its alarm goes off; heavy blows jolt the screen a bit.
  A fishing rod hooks cars and loose things too. Hurt in GTA, Steve flinches and you hear it.
- GTA's pickups (dropped money, weapons, health, armour) are collected in Minecraft movement too.
- A trident's riptide works in GTA's water (deep water too) and in GTA's rain, even with Minecraft movement off (F6):
  Minecraft flies you for the launch.
- Out of breath (empty hunger bar), GTA's own sprint waits too. Water douses a burning car for good.

## Settings

**`MCPassthrough.ini`** in the GTA V folder (made by `install.sh`; press F8 in GTA to reload it):

| setting | default | what it does |
|---|---|---|
| `LookSensitivity` | `1.0` | how fast the mouse turns the view in Minecraft movement (2.0 = twice as fast) |
| `InvertLook` | `0` | `1`: moving the mouse up looks down |
| `FreeLook` | `1` | `1`: Minecraft movement looks all the way up and down with its own camera; `0`: GTA's camera |
| `KeepMinimap` | `0` | `1`: Minecraft's blocks never cover GTA's minimap; `0`: they show there too |
| `Director` | `0` | `1`: obey the video tools' scripted shots (`tools/video`); leave it off for play |

**ReShade** (Home key in GTA → `MCPassthrough.fx`) has sliders for how Minecraft looks: lighting, gamma, saturation,
haze, glow, depth bias. `install.sh` keeps what you set (it only adds the effect to ReShade's list if it isn't there), and
`install.sh --remove` only removes the files it added itself (listed in `MCPassthrough.installed`).

## Troubleshooting

| problem | try |
|---|---|
| "Minecraft passthrough connected" never shows | Start Minecraft first and leave its window open; check `ScriptHookV.log` in the GTA folder |
| Minecraft's picture doesn't show | Press Home in GTA: ReShade must list `MCPassthrough.fx` as on. Run `FORCE=1 ./gta/install.sh` again |
| blocks float or sink | F8 re-levels the ground where you stand |
| walking feels wrong somewhere | F6 switches to GTA's own walking. It also switches by itself if GTA's floor can't be found |
| the mouse turns too fast or slow | change `LookSensitivity` in `MCPassthrough.ini`, then F8 |
| a mission needs GTA's guns | In a mission, drawing Minecraft's bow aims GTA's gun along with it (the prologue's hostages react). Tab switches Steve's hands to GTA's weapons |
| "GTA failed during ..." on screen | that frame was skipped and the plugin carries on; send `MCPassthrough.log` (it names the step that failed) |
| something doesn't work (leads, stops while walking, ...) | send `MCPassthrough.log` from the GTA folder: it lists every lead click and what it hit, each time Minecraft's player was put back (and why), ground re-levels and `/kill` results |

---

## How it works

```
GTA V (story mode)                                    Minecraft 26.3 + Fabric (mc/)
  MCPassthrough.asi (gta/src)                           dev.rehan.passthrough
    script.cpp  -- WebSocket 127.0.0.1:25599 ------->     HostLink: cam, ground, hc, gwater, key, cmd, ...
                <------------------------------------     events: explosions, projectiles, mobs, melee, ...
    compositor.cpp (ReShade add-on)  <-- shared memory --  FrameExporter: world RGBA + depth, overlay RGBA
    MCPassthrough.fx: depth test, relight, overlay        "Local\MCPassthroughFrame"
```

Coordinates: 1 GTA metre = 1 block. GTA (x, y, z) → Minecraft (x, z + yOffset, −y); Minecraft yaw = 180 − heading,
pitch = −pitch. The script picks yOffset so the ground where the player stands lands on a whole block.

- **Every GTA frame** the script sends the camera, the player's feet and Steve's pose (`cam`). The pose comes from
  GTA's bones while GTA moves the character, and includes GTA's health, armour and stamina and whether the player has
  control. Minecraft renders from that camera with no sky, fog or clouds.
- **Ground.** The script probes GTA's ground in the columns around the player and sends them as `ground`; the mod fills
  them with invisible barrier blocks, raised over GTA's walls and fences so mobs bump into them. GTA's water goes as
  `gwater`: Minecraft treats it as water nobody sees (`HostWater`, via `Level.getFluidState`; Minecraft only looks
  for water in chunk sections that hold some, so `EntityFluidMixin` lets it look where GTA has water).
- **Minecraft movement.** GTA's player is frozen (but not while standing still on GTA's ground) and follows Minecraft's. GTA's floors, walls (24 rays at three
  heights, joined) and ceilings around the player go to Minecraft as collision boxes (`hc`). Fast moves (gliding) are
  checked against GTA's world along their whole path.
- **Frames.** The mod copies Minecraft's world colour and depth before the hand is drawn, then the hand, HUD and
  screens as an overlay, into shared memory. The ReShade add-on uploads the newest frame, and `MCPassthrough.fx`
  re-projects it to GTA's current camera, draws it where it's nearer than GTA's depth, and relights it from GTA's
  surfaces around it.
- **Events back.** Explosions, projectiles in flight (traced through GTA's world), swings with their enchantments,
  ender pearls, mobs, water, fire, commands and player damage go to the script, which acts them out in GTA.

## Files

| path | what it is |
|---|---|
| `mc/` | the Fabric mod (Java 25, Loom): `HostLink` (WebSocket), `FrameExporter`, `PlayerSync`, `SteveRig` (Steve posed from GTA's bones), `HostBars` (HUD), `WorldBridge` (barriers, blocks, explosions, projectiles), `HostBridge` (animals, mobs, fires, commands, player sync), `HostWater`, `HostCollision`, `MobWar`, `Nether`, `TheEnd`, and the mixins |
| `gta/src/script.cpp` | the ScriptHookV script: camera, ground, input, movement, free look, flying cars, melee, projectiles, mobs, fires, the Nether, the End |
| `gta/src/compositor.cpp` | the ReShade add-on: uploads Minecraft's frame and sets the effect's uniforms |
| `gta/src/natives.h` | the GTA natives the script calls, by hash |
| `gta/shaders/MCPassthrough.fx` | the ReShade effect |
| `gta/fetch_deps.sh`, `build.sh`, `build.bat`, `install.sh` | fetch ScriptHookV and ReShade, build with MSVC, install into the game folder |
| `gta/tests/` | `check_natives.py` (every native's hash and argument count against the native DB), `fakegta.cpp`, `ws_test.cpp` |
| `tests/sim/` | a GTA simulator: the script compiled on Linux against a box world, with mocked natives |
| `tests/*.py`, `tests/RigCheck.java` | Minecraft-side integration tests (a fake GTA over the link) and the rig maths check |
| `CHANGELOG.md` | what changed in each pass |
| `tools/host/`, `tools/video/` | older test tools and the video/director pipeline (the plugin obeys the director only with `Director=1` in MCPassthrough.ini) |
| `dist/passthrough-0.1.0.jar` | the built Minecraft mod |

## Requirements

- **Windows 10/11 with WSL** (Ubuntu). The scripts are bash (run them in WSL) and batch.
- **Minecraft Java Edition 26.3**, Fabric Loader 0.19.5 or newer, and Fabric API 0.161.0+26.3 (Prism Launcher works).
- **GTA V Legacy** on Steam, story mode (`GTA5.exe`; the Enhanced edition isn't supported).
- **ScriptHookV** and its ASI loader, and **ReShade 6.8.0 with add-on support**. `gta/fetch_deps.sh` downloads both.
  ScriptHookV only runs on game builds it supports: after a GTA update, wait for a new ScriptHookV.
- **Visual Studio 2022 or newer** with the C++ desktop tools (x64).
- **JDK 25** (to build the mod yourself).

## Building from source

- **The mod:** `cd mc && ./gradlew build` (JDK 25) builds `mc/build/libs/passthrough-0.1.0.jar`. From WSL with a
  Windows JDK, `./gradle.sh build` builds it in `PASSTHROUGH_WIN_DIR` (default `C:\dev\passthrough`).
- **The GTA side:** `gta/fetch_deps.sh`, then `gta/build.sh` (mirrors `gta/` to `<PASSTHROUGH_WIN_DIR>\gta` and runs
  MSVC there), then `gta/install.sh` (`GTA_DIR` overrides the Steam lookup; `install.sh --remove` takes everything out
  again).

## Tests

None of these need GTA:

```bash
# every native call's hash and argument count against alloc8or's native DB
curl -sLO https://raw.githubusercontent.com/alloc8or/gta5-nativedb-data/master/natives.json
python3 gta/tests/check_natives.py natives.json

# the GTA simulator (the script's movement, input, rig, minimap mask, arrows, character switches, leads, boats,
# pickups and re-levelling never stopping the player, punched signs flung, /kill on people in cars,
# fire)
python3 tests/sim/gen_hashes.py
g++ -std=c++20 -w -Itests/sim/inc -Igta/third_party/shv "-D__declspec(x)=" -DNOMINMAX tests/sim/sim.cpp -o sim && ./sim

# Minecraft side: start the dev client (cd mc && ./gradlew runClient), then
python3 tests/scratchpad_test_host.py   # screens, portals, melee, projectiles
python3 tests/walktest.py               # Minecraft movement against GTA's collision, pearls, blocks on walls, water
python3 tests/features_test.py          # animals, commands, mobs, fires, spectator, HUD health, cutscene input, mission restart, /range, Straight
python3 tests/leads_test.py             # leads on GTA's things (held, tied, cut), Minecraft's mobs tied there, boat seats
python3 tests/water_test.py             # in GTA's water: in water (swimming), and a riptide trident launches there
python3 tests/fall_test.py              # falls at GTA's speed and hurt, water landings don't, the void doesn't kill, mobs hunt you in creative
```

The GTA side is also compile-checked with MinGW (`x86_64-w64-mingw32-g++ -fsyntax-only`), and the effect with
ReShade's own FX compiler.

## Safety

- **Story mode only; never GTA Online.** This runs with BattlEye off, which keeps Online from starting, and ScriptHookV
  closes the game if it goes online anyway.
- **Don't automate clicks on GTA's landing page.** Pick Story Mode by hand.
- `install.sh` lists what it adds, won't replace another mod's loader or ReShade without `FORCE=1`, and `--remove`
  takes it all out again.
- The link listens on 127.0.0.1:25599 only, with no token: any program on the machine can send it commands. Close
  Minecraft when you're done.
- ScriptHookV and ReShade are downloaded from their own sites and aren't redistributed here, and neither is anything
  from Minecraft or GTA.

## Credits

- [ScriptHookV](https://www.dev-c.com/gtav/scripthookv/) and its ASI loader by Alexander Blade.
- [ReShade](https://reshade.me) and its add-on API by crosire.
- [Fabric](https://fabricmc.net): Fabric Loader, Fabric API and Loom.
- GTA V native names and hashes from alloc8or's [native DB](https://github.com/alloc8or/gta5-nativedb-data).
- [Java-WebSocket](https://github.com/TooTallNate/Java-WebSocket) by TooTallNate, bundled for the link.
- The original passthrough example by rehan ([universal-modder](https://github.com/rehan-remade/universal-modder)),
  inspired by chasm's Minecraft-in-Skyrim and TobynJacobs' Minecraft-in-Elden-Ring.
- Written with Claude Code.
- Minecraft belongs to Mojang Studios and Microsoft, and GTA V to Rockstar Games and Take-Two. This is a fan project.
