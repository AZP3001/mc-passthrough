# Changelog

## Story-mode stability and crossover pass (2026-10-06)

Tested in the GTA simulator (`tests/sim`, all checks, including four new ones that fail on the previous version)
and in Minecraft (`tests/*_test.py`, `walktest.py`, `scratchpad_test_host.py`, all passing). Not yet tried in
real GTA.

### Story mode and stability

| Fixed | What was wrong | Now |
|---|---|---|
| Missions whose enemies, guards or police must spot you | The player character was hidden with `SET_ENTITY_VISIBLE(false)`, which also hides it from GTA's AI, so scripted fights, chases and alarms could fail to start | Hidden only from view, a frame at a time (`SET_ENTITY_LOCALLY_INVISIBLE`). GTA's people still see you, and a mission showing or hiding the player isn't fought |
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
