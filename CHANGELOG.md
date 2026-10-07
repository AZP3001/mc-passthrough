# Changelog

## Steve 85%, faster and exact arrows, arrows into cars, scroll zoom (2026-10-07)

Tested in the GTA simulator (new check for arrows into cars) and in Minecraft (`features_test.py`, new arrow speed check,
all passing). Not yet tried in real GTA.

| Change | Now |
|---|---|
| Steve 15% smaller | Drawn at 85% of the GTA character's height (was 70%). The hitbox is unchanged |
| Faster arrows | A player's bow and crossbow arrows leave 3x as fast, still scaled by how far the bow was drawn. Damage in Minecraft stays the same |
| Arrows at people in cars | GTA's line meets the car, never the one inside, so the arrow hit the car or nothing. Whoever sits on the arrow's path, up to 3 m past where it met the car, is hit now, and a window on the way breaks |
| Bow dead accurate | Minecraft scattered a bow's (and crossbow's) arrows a little; they now leave exactly where you look. Multishot's side arrows still fan out |
| Scroll to zoom | While holding Ctrl, the mouse wheel zooms further in or out (the hotbar doesn't change). Let go and the next zoom starts at a quarter again |
| Angry drivers | Drivers who drive off angry from a drawn bow now floor it (honk longer, run lights), so you can tell they reacted |

## Range, bow, lava, water, zoom (2026-10-06)

Tested in the GTA simulator (all checks, new ones for each row marked *) and in Minecraft (`fall_test.py`,
`features_test.py`, `water_test.py`, all passing). Not yet tried in real GTA.

| Change | Now |
|---|---|
| Arrows missed people far off* | GTA gives people more than a street away no collision for line probes, so arrows flew through them. An arrow now also hits whoever is within a hand's breadth of its path. Melee's same fallback is a little wider |
| `/range` forgotten | If the GTA script restarted, its reach went back to 8 while Minecraft kept yours. Minecraft now sends it again on every connect |
| Bow damage* | Two body hits kill a person, one to the head (an arrow was a sniper bullet's worth: one hit anywhere) |
| Bow at cars | The driver reacts as to a gun in GTA: most honk and drive off angry, some back off and flee, a few give up (everyone out, hands up), and now and then one drives at you. Police and armed people only look |
| Ctrl zoom* | Hold Ctrl in Minecraft's movement to zoom to a quarter of the view, mouse slowed to match |
| Riptide in freefall | A riptide during GTA's skydive (in the rain) now takes over: GTA's skydive had kept Minecraft's movement off |
| Lava on cars* | Only the tyres in the lava pop (it popped them all). The car dents where the lava touches it, at body height, so it shows. It's pushed out harder, and through if it sits in lava all over |
| Water push* | Cars and loose objects are eased toward the water's pace every frame (they were shoved four times a second and lurched). You're only carried while on your feet, so jumps and falls aren't cancelled |
| Built blocks far off* | Blocks get GTA collision up to 100 m away (was 30 m), and blocks buried on all sides get none, so the 400-prop budget reaches further. GTA's own cars far from you may still drive through: GTA runs distant traffic without physics |
| GTA's animals in `/summon` | All of GTA's animals (`a_c_deer`, `a_c_mtlion`, `a_c_sharktiger`, ...) are listed as you type `/summon`, plus `animal` for a random land one (`/gta spawn animal 5` too). They spawn as GTA animals, not as people with an animal's model |
| GTA water | Minecraft movement's "in GTA's water" checks know rivers now. The log (`MCPassthrough.log`) notes GTA's water sent to Minecraft, arrow splashes, riptides, and going in and out of water, to find why swimming and riptide fail in real GTA |

## Water push, smaller Steve, spawn fix (2026-10-06)

Tested in the GTA simulator (all checks) and in Minecraft (`fall_test.py` with a new spawn check, `features_test.py`,
`water_test.py`, all passing). Not yet tried in real GTA.

| Change | Now |
|---|---|
| Instant death on spawn | A fall in progress when GTA put you somewhere new (a respawn, the world loading in under you) counted when you landed (74 damage in the test). A host teleport now clears the fall, and a landing within 1.5 s of one doesn't hurt |
| Water pushes | Flowing Minecraft water carries you (GTA's movement; Minecraft's movement already was) and GTA's loose objects (bins, cones, boxes), as well as people and cars |
| Steve 30% smaller | Drawn at 70% of the GTA character's height. The hitbox is unchanged |
| Arrows half size | Drawn at half size. The hitbox is unchanged |
| Steve seen through things | Steve no longer gets the extra depth allowance at GTA's edges, and the "in a car, the car's pillars let him show" rule is gone. Glass is now checked per cell (a 5 x 8 grid of lines from the camera to him): he shows through a car's or shop's window only where the first thing a line meets is glass, and only on that glass |
| Arrow hit sound | Arrows and tridents landing in GTA's world (walls, ground, people, cars) play Minecraft's hit sound |

## Round 2: falls, mobs, leads, missions (2026-10-06)

Tested in the GTA simulator (`tests/sim`, all checks) and in Minecraft (`tests/*_test.py`, `walktest.py`,
`scratchpad_test_host.py`, all passing; new `tests/fall_test.py`). Not yet tried in real GTA.

### Removed

- **Mining GTA's world** with a pickaxe is gone (GTA side, shader and Minecraft side), with the pits cars and people
  fell into. The five blocks it used stay registered, unseen and inert, so a world that had them still loads without
  Fabric's "missing content" screen.

### Fixed

| Fixed | What was wrong | Now |
|---|---|---|
| No fall damage | The mod lets you fly (`/fly`), and Minecraft spares anyone allowed to fly. A fall that would have killed Minecraft's player also killed it in Minecraft only, and the report to GTA skipped dead players | Falls hurt GTA's health. Minecraft's player is held at one heart and the full damage goes to GTA, which decides when you die. The same fix makes fire, drowning and mobs hurt you |
| Mobs ignored you | The mod's world is creative, so Minecraft's mobs never targeted you, and fire, water and falls never hurt | While GTA is linked, creative plays like survival for danger: mobs hunt and hit you, fire burns, water drowns, falls hurt. Creative's building and inventory stay. Hunger stays full (the bar shows GTA's stamina) |
| Riptide fell through floors | At a riptide's (or a long fall's) speed, GTA's floor reached Minecraft too late | The whole path fallen each frame is checked for a floor, wherever you'd stood before |
| Died in the void, no way back up | Minecraft's void killed you, and in Minecraft movement GTA never put you back up | The void never kills you. Fallen out of GTA's world (nothing under you, GTA's ground overhead) you're put back on its ground, or where you last stood |
| Bow drawn but wouldn't shoot | In a mission, a drawn bow holds GTA's aim down itself, so the right button's release never showed | The mouse buttons are read as they really are |
| Knockback killed instead of throwing | The blow killed first, and the push never moved a dead body | Pushed first, hurt a moment later: they fly, even from a killing blow. Punch and tridents too |
| Punching signs did nothing | The copy of a sign made in place of the map's had no physics yet the frame it was pushed, and a not-yet-loaded model lost the hit | The push is given again until it moves, and waits for the model. Thin poles are found with a fatter line. Punch arrows fling signs, bins and lamp posts too |
| Things duplicated when hit | A sign hit again before GTA hid the original got a second copy | One copy per thing |
| Leads snapped, people circled you | Leads snapped past 11 blocks; a pull kept the sideways speed, so the pulled spun round the puller, and people were lifted (floating) | Leads never snap (after a teleport, what you lead comes along). Sideways speed is damped and people are dragged along the ground on their back |
| Mission triggers in Minecraft movement | GTA's player was always frozen | Not frozen while you stand still on GTA's ground |
| Steve dark in cutscenes | Steve took the light of the room round him, often dark | In cutscenes Steve is lit at least as GTA lights its characters (`SteveMinLight`) |
| Minecraft's water hid the street | Water (70% covered) counted as solid | Water, glass and ice blend with GTA again (`SolidCoverage`, 0.85) |
| Jump in mid-air | Holding Space by a wall in a fall climbed it, from where you'd last stood | Climbing only from the ground (or the jump just taken from it) |
| `/clearall` killed GTA's people and cars | It cleared GTA's whole area round you too (a mission's people and cars with it) | It clears only Minecraft's things and puts back what of GTA's you moved (signs, bins, doors knocked loose); GTA's cars and people stay |
| Too much health | A totem's hidden 2000 health, left on by a save or a reload, never came off | Back to the story characters' 200 |

### Added

| Feature | Details |
|---|---|
| Straight | A bow-only enchantment (one level): its arrows fly dead straight, no gravity, and are gone after 500 blocks |
| Bow intimidation | Cops, soldiers and anyone fighting only look at you. Pointed at a car, only that car's people get out with their hands up. Crossbows too |
| GTA's fall | Falls speed up as GTA's do (9.8 m/s per second, up to about 52 m/s). A long fall with a parachute hands over to GTA's skydive (F opens it) |
| Slow motion | GTA's slow motion (dying, Michael's and Franklin's abilities) slows Minecraft too |
| Mission restart | Restarting a mission or going back to a checkpoint clears Minecraft's things; the mission's own cars and people stay |
| Doors | Unlocked doors open as you walk through them in Minecraft movement (locked ones stay walls) |
| Q | Always GTA's cover; Minecraft's drop is off, in the inventory too |
| C | Sneak (and fly down) |
| GTA's phone | Minecraft's hand and HUD make way for it, and the clicks and wheel go to it |
| Drowning | GTA's people with their heads in Minecraft's water drown (15 s of breath) |
| Leads | Two of GTA's people or animals tied together (a dog and a person) pull each other by weight. Minecraft's own leads never snap either |
| `/range` | Up to 512 blocks |

### GTA natives newly used

| Native | Hash | What for |
|---|---|---|
| `TASK_SKY_DIVE` | `0x601736CFE536B0A0` | A long fall with a parachute |
| `IS_PED_RUNNING_MOBILE_PHONE_TASK` | `0x2AFE52F782F25775` | GTA's phone out |
| `IS_SPECIAL_ABILITY_ACTIVE` | `0x3E5F7FC85D854E15` | Slow motion |
| `GET_STATE_OF_CLOSEST_DOOR_OF_TYPE` | `0xEDC1A5B84AEF33FF` | Doors, locked or not |
| `START_SHAPE_TEST_CAPSULE` | `0x28579D1B8F8AAC80` | Thin things by the crosshair |
| `IS_PED_IN_COMBAT`, `IS_PED_ARMED`, `TASK_LOOK_AT_ENTITY`, `TASK_TURN_PED_TO_FACE_ENTITY` | | Bow intimidation |

Every native call is checked against alloc8or's native database (`gta/tests/check_natives.py`: 356 calls, 0
problems).

## Hotfix (2026-10-06)

- The player character is hidden for real again (`SET_ENTITY_VISIBLE(false)`, plus `SET_ENTITY_LOCALLY_INVISIBLE`).
  Hidden only from view, Franklin's or Michael's head was drawn in first person and showed through Steve.
- T (chat), E and I (inventory) open Minecraft's screens again. The check meant to skip them while GTA's own on-screen
  keyboard or phone was up read as "typing" all the time and swallowed them, so it was removed.

## Story-mode stability and crossover pass (2026-10-06)

Tested in the GTA simulator (`tests/sim`, all checks, including four new ones that fail on the previous version)
and in Minecraft (`tests/*_test.py`, `walktest.py`, `scratchpad_test_host.py`, all passing). Not yet tried in
real GTA.

### Story mode and stability

| Fixed | What was wrong | Now |
|---|---|---|
| Camera lock after a mission's scripted camera | In Minecraft movement, when a mission's own camera ended, GTA went back to its gameplay camera, which the plugin holds still, so the view froze | The free-look camera takes the view back as soon as no other script camera is rendering. A mission's camera is never overridden while it renders |
| Vehicle weapons in Minecraft-hands mode | A Buzzard's rockets, a tank's cannon and other vehicle-weapon keys were blocked, and the mouse went to Minecraft's item | In a vehicle with weapons, GTA's vehicle-weapon keys work and the mouse fires them. Keys held in Minecraft are released on the way in |
| Cover (Q) in missions | Q was always Minecraft's drop, so "press Q to take cover" never worked | Q takes cover while GTA's help text is up or you're already in cover; otherwise it's Minecraft's drop |
| Drowning twice | In GTA's own movement, Minecraft also counted down its own air and drowned the player on top of GTA's drowning | GTA's remaining breath is Minecraft's air bubbles (per character's lung capacity), and Minecraft never drowns the player itself then. In Minecraft movement, Minecraft's breath is in charge as before |
| Map that never shrank | The list of people recently let out of a Minecraft boat grew for the whole session | Trimmed to the last few seconds |

### Water and ballistics

| Added or fixed | Details |
|---|---|
| Fishing in GTA's sea, lakes and rivers | A bobber cast into GTA water was traced down to the seabed and deleted there, so fishing never worked. It now floats on the surface and Minecraft fishes in it normally. Anything above the water on the way still gets hooked |
| Splashes | Arrows, tridents, pearls, bobbers and thrown things that go into GTA water make GTA's own splash there. The splash is a harmless shot with no owner, so it doesn't count toward a wanted level |
| Impaling | A trident with Impaling hits GTA's people and cars 40% harder per level when they're in water or out in the rain (Bedrock's rule) |
| Safe ender-pearl landing | The landing spot is checked for room for Steve (0.6 m wide, 1.8 m tall) and nudged out of whatever is in the way: off the wall it hit, under a ceiling, on top of a floor or slope. Up to a metre, so you never end up inside geometry |
| Riptide, Loyalty, Channeling | Riptide carries the player in GTA water (fixed last release). Loyalty tridents stuck in GTA's world or people come back. Channeling strikes lightning where the trident lands |

### New crossover features and polish

| Feature | Details |
|---|---|
| Hit markers | A swing, arrow or trident that lands on GTA's people or cars shows a marker round the crosshair. It's white for a hit and red for a kill, and fades out |
| Hit sounds | Minecraft's own sounds: an arrow's "ding" for shots, a strong swing's thud for melee, and the XP chime for a kill |
| XP for kills | Someone you hurt who dies within 8 s is your kill. Experience orbs drop where they fell: 5 for a person, 8 for a cop or soldier, 2 for an animal |
| Smoother third-person camera | When a wall or lamp post pulls the free-look camera in, it now eases back out once the obstacle is gone, instead of jumping. It still comes in instantly, so it never clips through walls |

### GTA natives newly used

| Native | Hash | What for |
|---|---|---|
| `SET_ENTITY_LOCALLY_INVISIBLE` | `0xE135A9FF3F5D05D8` | Hiding the player only from view, each frame |
| `GET_RENDERING_CAM` | `0x5234F9F10919EABA` | Seeing when a mission's camera has ended |
| `DOES_VEHICLE_HAVE_WEAPONS` | `0x25ECB9F8017D98E0` | Giving vehicle-weapon keys back to GTA |
| `IS_PED_IN_COVER` | `0x60DFD0691A170B88` | Keeping Q as cover while in cover |
| `GET_PLAYER_UNDERWATER_TIME_REMAINING` | `0xA1FCF8E6AF40B731` | GTA's breath for Minecraft's air bubbles |
| `IS_PED_SWIMMING_UNDER_WATER` | `0xC024869A53992F34` | Under water or not, for the breath |
| `IS_ENTITY_IN_WATER` | `0xCFB0A0D8EDD145A3` | Impaling's wet targets |
| `SHOOT_SINGLE_BULLET_BETWEEN_COORDS` | `0x867654CBC7606F2C` | Water splashes (no damage, no owner) |

Every native call is checked against alloc8or's native database (`gta/tests/check_natives.py`: 347 calls, 0
problems).

### Files changed

- `gta/src/script.cpp`
- `gta/src/natives.h`
- `mc/src/client/java/dev/rehan/passthrough/client/HitMarker.java` (new)
- `mc/src/client/java/dev/rehan/passthrough/client/mixin/HudMixin.java`
- `mc/src/client/java/dev/rehan/passthrough/client/HostLink.java`
- `mc/src/client/java/dev/rehan/passthrough/client/HostState.java`
- `mc/src/client/java/dev/rehan/passthrough/client/PassthroughClient.java`
- `mc/src/main/java/dev/rehan/passthrough/WorldBridge.java`
- `mc/src/main/java/dev/rehan/passthrough/HostBridge.java`
- `mc/src/main/java/dev/rehan/passthrough/Passthrough.java`
- `tests/sim/sim.cpp`
- `tests/water_test.py`
- `dist/passthrough-0.1.0.jar`

### Already in place, not changed

- Flame bows set GTA's people and cars on fire, and Punch knocks them down.
- Ender pearls teleport you, or your car.
- Driving and flying use GTA's own controls. A flying car follows Minecraft's flight.
- Minecraft boats carry GTA's people.
- The elytra flies GTA's player.
- Health, armour and stamina show on Minecraft's HUD.
- GTA damage makes Steve flinch with Minecraft's hurt sound.
