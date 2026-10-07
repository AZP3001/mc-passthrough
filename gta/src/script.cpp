// ScriptHookV half: every game frame, send GTA's camera and player to the Minecraft mod, feed it the ground
// around the player (as barrier columns), forward the mouse to Minecraft, hide GTA's own player, and turn
// Minecraft's explosions into GTA explosions.
//
// Coordinates: 1 GTA metre = 1 block. GTA (x, y, z) with z up -> Minecraft (x, z + yOffset, -y).
// Minecraft yaw = 180 - GTA heading, pitch = -GTA pitch. yOffset puts the local ground on a whole block.
//
// Keys: F7 toggles the passthrough, F8 re-levels the Minecraft ground to where the player stands, F6 Minecraft
// movement on/off, F9 god mode (no police, can't die) on/off, Tab Minecraft items / GTA weapons, E (or I)
// Minecraft's inventory, T Minecraft's chat, Space at a high ledge climbs it, F at a ladder climbs it.
#include "compositor.h"
#include "natives.h"
#include "ws.h"
#include <main.h>
#include <algorithm>
#include <atomic>
#include <climits>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <functional>
#include <string>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
	constexpr int kPort = 25599;
	constexpr int kGroundRadius = 40;        // blocks around the player that get collision
	constexpr int kGroundProbesPerTick = 110;
	constexpr int kGroundDepth = 2;          // barrier layers under each surface
	constexpr Hash kWeaponUnarmed = 0xA2719263;
	// The director's gun op: GTA's own weapons in Steve's hands. Carbine rifle, minigun, RPG.
	constexpr Hash kGuns[] = {0x83BF0278, 0x42BF8A85, 0xB1CA77B1};
	// Tab: Steve's hands hold GTA's own weapons (aim, shoot, mouse wheel and number keys pick one) instead of
	// Minecraft's items (then GTA's mouse buttons, wheel and number keys go to Minecraft)
	bool g_gtaHands = false;
	bool g_missionAim = false; // GTA's player aims its gun along with Minecraft's drawn bow (in a mission)
	Hash g_gtaWeapon = 0; // the GTA weapon to take out again
	// Police mode: a wanted level that stays (director)
	bool g_police = false;
	// God mode (F9): the player can't die and the police leave them alone. Off: GTA's own rules (story missions).
	bool g_godMode = false;
	// Minecraft's movement (F6). On foot Minecraft moves the player (its walking, sprinting, sneaking, jumping, swimming,
	// ladders, elytra, ...) and GTA's player (frozen, invisible) follows it, while GTA's floors, walls and ceilings
	// around the player go to Minecraft as collision boxes (proxy_tick). GTA moves the player itself where Minecraft
	// can't: vehicles, ragdolls, ledges and ladders (climbed GTA's way), GTA's water, cover, parachutes, and missions
	// taking control. Off: GTA's own movement.
	bool g_mcMove = true;
	struct Walk
	{
		bool on = false;         // Minecraft moves the player now
		bool acked = false;      // Minecraft has said so (its positions count from then on)
		int gtaUntil = 0;        // GTA keeps the player until then (a climb, a ladder, a car about to hit)
		float x = 0, y = 0, z = 0; // the player's feet this frame (GTA coordinates)
		float standZ = 0;        // the feet when last on the ground (floor probes start above it)
		bool ground = true;      // Minecraft: on the ground
		int teleportAt = -100000; // Minecraft teleported the player (a pearl): a jump in its position is expected
		int nextFellCheck = 0;
		int correctedAt = -100000;
		int psetId = 0;          // the last position GTA put Minecraft's player at (Minecraft echoes it)
		int fellAt = -100000;    // when Minecraft's player last fell through GTA's floor
		Ped frozen = 0;          // the ped frozen for it (a character switch hands another one over)
		int forceUntil = 0;      // Minecraft moves the player until then whatever (a trident's riptide launched it)
		float ex = 0, ey = 0, ez = 0; // where the player is drawn: carried by Minecraft's velocity every frame, eased onto
		bool haveEst = false;         // its samples (they come 20 times a second, unevenly: snapping to each stuttered)
		int64_t estAt = 0;
		float putX = 0, putY = 0, putZ = 0; // where GTA's player was last put (by us: at Minecraft's player, or on a pickup)
		int groundAt = 0;        // when Minecraft's player was last on the ground
		int nextVoidCheck = 0, voidSince = 0; // fallen out of GTA's world (nothing under, its ground overhead): since when
		float safeX = 0, safeY = 0, safeZ = 0; // the last place it stood (where it goes back to if no ground is found)
		bool haveSafe = false;
	} g_walk;
	// Cutscenes, character switches, loading screens, fades, warnings and missions taking control: Minecraft gets no
	// input (a click forwarded mid-cutscene could hit someone the mission needs), and Minecraft's swings, arrows, mob
	// hits and explosions leave GTA's world alone until it's over.
	bool g_noInput = false;
	Ped g_lastPed = 0; // the player's character last frame (a switch to Michael, Franklin or Trevor changes it)
	// Minecraft's free look while Minecraft moves the player (walk_cam_tick): a camera of our own that looks all the way
	// up and down, as Minecraft's does (GTA's stops well short of straight down, which bridging and pillaring need)
	struct WalkCam
	{
		Cam cam = 0;
		float heading = 0, pitch = 0;
		int mode = 1;      // GTA's view mode, cycled by its view key (V): 0..2 third person, near to far; 4 first person
		int64_t last = 0;
		float fov = 50.0f; // GTA's, as the camera took over: kept (GTA's own widens and narrows as the player speeds up)
		float pull = -1.0f; // third person: how far behind Steve it is now (eased back out after a wall pulled it in)
	} g_walkCam;
	// MCPassthrough.ini next to MCPassthrough.asi ([Minecraft] LookSensitivity=1.0, InvertLook=0, FreeLook=1)
	struct Settings
	{
		float lookSensitivity = 1.0f;
		bool invertLook = false;
		bool freeLook = true;
		bool keepMinimap = false; // Minecraft's world never covers GTA's minimap (off: blocks show there too)
		bool director = false;    // the video tools' scripted shots ({"t":"gta","op":...}) obeyed (Director=1; off for play)
	} g_settings;
	int g_spaceDownAt = INT_MAX; // Space held since then (held at a ledge too high to jump, GTA climbs it)
	// A car flying with Steve (Minecraft's movement on, in the driver's seat, Space twice): Minecraft flies the player
	// (creative flight) and the car goes where the player is, facing where the camera looks (car_fly_tick)
	struct CarFly
	{
		bool on = false;
		Vehicle car = 0;
		float ox = 0, oy = 0, oz = 0; // the car's centre from the player's feet, in the car's frame (right, forward, up)
		int lastSpace = -100000;
		int startedAt = 0;
		int teleportAt = -100000;
	} g_carFly;
	bool g_mcSpectator = false;   // Minecraft's player is a spectator (/spectator): GTA's player flies through everything
	bool g_spectatorOn = false;   // what was last applied to GTA's player for it
	bool g_mcFlying = false;      // Minecraft's player glides (elytra) or flies: its whole way is checked for GTA's walls
	// Every Minecraft mob near the player (Minecraft coordinates), for GTA's world to hit (cars, flung people) and burn
	struct WorldMob
	{
		int id;
		float x, y, z; // GTA coordinates, feet
		float width, height;
		bool fire;
	};
	std::vector<WorldMob> g_worldMobs;
	int g_nextWorldMobsAt = 0, g_nextFiresAt = 0;
	std::string g_firesSent;
	// Minecraft's animals that live in GTA too (a cow, a pig, a chicken, ...): spawned as GTA's own, queued until the
	// model is in
	struct AnimalSpawn
	{
		Hash model;
		int pedType;
		float x, y, z, heading;
		int until;
	};
	std::vector<AnimalSpawn> g_animalQueue;
	std::unordered_map<Vehicle, Vector3> g_carVelocity; // last frame's, to see a crash into Minecraft's blocks
	// GTA's collision near the player, as boxes for Minecraft (GTA coordinates). Floors: a grid of 0.5 m cells probed
	// from above the standing height. Walls: boxes behind what a fan of rays hits at knee and chest height. Ceilings:
	// boxes over the head.
	struct HostBox
	{
		float x0, y0, z0, x1, y1, z1;
		int at; // game time found
	};
	struct FloorCell
	{
		float top;
		int at, i, j; // when probed; the cell (kCell grid)
	};
	struct Proxy
	{
		std::unordered_map<int64_t, FloorCell> floor;             // cell -> its top (NaN: nothing below)
		float floorBase = -100000.0f;                             // the standing height the cells were probed from
		std::unordered_map<int64_t, HostBox> walls;               // by 0.25 m spot (and ring)
		std::unordered_map<int64_t, HostBox> ceilings;
		int ring = 0, around = 0;
		int nextSendAt = 0;
		bool dirty = false;
	} g_proxy;
	constexpr float kCell = 0.5f;   // floor cell size (m)
	constexpr int kCellRadius = 6;  // cells each way around the player
	int g_lastSpace = -100000;      // Space twice quickly opens the elytra (GTA's movement, elytra worn)
	bool g_mcElytra = false;        // Minecraft: the player wears an elytra
	bool g_mcHudHidden = false;     // Minecraft's HUD hidden (cutscenes)
	// GTA's own HUD (minimap, help text, mission text); the director hides it for clean video
	bool g_showHud = true;
	// Minecraft's open screen (it tells us): 0 none, 1 chat, 2 inventory/container, 3 other. While one is open GTA
	// stands still, its mouse pointer shows, and mouse and keyboard go to Minecraft.
	std::atomic<int> g_screen{0};
	float g_cursorX = -1.0f, g_cursorY = -1.0f;
	bool g_mouseLeft = false, g_mouseRight = false;
	std::atomic<bool> g_wantChat{false}, g_wantInventory{false}, g_toggleGod{false}, g_toggleJump{false};
	// Minecraft's projectiles in flight (by entity id), traced segment by segment through GTA's world. kind: arrow,
	// trident, firework, snowball, egg, potion, wind, bobber; sub: the potion, or "chan" for a channeling trident.
	struct Projectile
	{
		float x, y, z;
		std::string kind, sub;
		bool seen;
		bool done; // hit something of GTA's already: no more tracing, and no burst when it goes
		float ux = 0, uy = 0, uz = 0; // which way it last flew
		bool wet = false; // gone into GTA's water (splashed there)
	};
	std::map<int, Projectile> g_projectiles;
	std::vector<Entity> g_army; // tanks, helicopters and their crews ("army" op), to clear them again
	// Minecraft's mobs vs GTA's people (mobs_tick). Minecraft lists its hostile mobs ("mobs"); each gets an invisible,
	// frozen GTA "double" ped in a group the police hate, so GTA's cops really shoot at them, and the double's lost
	// health goes back to the mob ("mobdmg"). GTA lists its people ("peds"); Minecraft gives each an invisible proxy
	// that its mobs hunt, and a mob's hit on a proxy comes back ("mobhit") as damage to the real person.
	struct MobDouble
	{
		Ped ped = 0;
		float x = 0, y = 0, z = 0; // the mob's feet, GTA coordinates
		int health = 0;             // the double's health after the last reset
		bool seen = false;
	};
	std::map<int, MobDouble> g_mobs; // by Minecraft entity id
	std::unordered_set<Ped> g_doublePeds;
	std::unordered_set<Ped> g_pedsSent; // the last "peds" list (a mobhit must name one of these)
	std::map<Ped, Ped> g_copTarget;    // cop -> the double it was told to fight
	std::vector<Ped> g_squad;          // spawned cops ("cops" op) and their cars
	std::vector<Vehicle> g_squadCars;
	int g_pendingCops = 0;
	float g_copsDist = 40.0f;
	bool g_copsLine = false; // park the squad broadside across the ground ahead (a barricade), not on the nearest road
	Hash g_mobGroup = 0;
	constexpr Hash kCopGroup = 0xA49E591C, kArmyGroup = 0xE3D976F3;
	int g_mobsSeenAt = -100000;
	int g_statHits = 0, g_statDmg = 0, g_statDoubles = 0; // debug counters (mobinfo)
	int g_nextPedsAt = 0, g_nextCopTaskAt = 0;
	// Minecraft explosions GTA mirrors: they already hurt Minecraft's mobs, so the doubles' losses from them don't count
	struct RecentBoom
	{
		float x = 0, y = 0, z = 0;
		int until = 0;
	} g_booms[8];
	int g_boomNext = 0;
	int g_doubleVis = 0;              // 0 hidden (SetEntityVisible), 1 locally invisible per frame, 2 alpha 0, 3 shown (debug)
	float g_mobHitScale = 12.0f;      // Minecraft damage (hearts x2) -> GTA damage
	float g_mobDmgScale = 0.125f;     // GTA damage -> Minecraft damage
	constexpr int kDoubleHealth = 5000;
	constexpr size_t kMaxDoubles = 48;
	bool g_huntCopsOnly = false;      // Minecraft's mobs hunt only the police (and army), not passers-by
	// The Nether opening ("nether" from Minecraft): hell comes to GTA's world (hell_tick), Minecraft's hot blocks set
	// GTA's people and cars on fire (hot_tick) and light GTA's world (hell_lights).
	struct Hell
	{
		bool on = false;
		int t0 = 0;
		int fromMinutes = 0;
		bool locked = false, shaking = false;
		int nextCopsAt = 0;
		float x = 0, y = 0, z = 0; // the portal's centre, GTA coordinates
	} g_hell;
	// The End opening ("end" from Minecraft): midnight and fog over the city, endermen and the dragon (Minecraft's),
	// a purple glow at the portal.
	struct TheEnd
	{
		bool on = false;
		int t0 = 0;
		int fromMinutes = 0;
		bool locked = false;
		float x = 0, y = 0, z = 0;
	} g_end;
	// GTA's clock before a realm (the Nether, the End) took it over, to put back when it closes
	int g_realmClock = -1;
	struct HotCluster
	{
		float sx = 0, sy = 0, sz = 0; // sums of cell centres, Minecraft coordinates
		int n = 0;
		int kind = 0; // 0 lava, 1 fire, 2 soul fire
	};
	std::unordered_map<int64_t, int> g_hot;              // Minecraft cell -> kind
	std::unordered_map<int64_t, HotCluster> g_hotClusters; // 4x4-column groups of hot cells, per kind (lights)
	int g_nextHotCheckAt = 0;
	// Minecraft's water: its cells (Minecraft coordinates) and the way each flows (x, z, thousandths), to put out GTA's
	// fires and sweep GTA's people along (water_tick)
	std::unordered_map<int64_t, std::pair<short, short>> g_water;
	std::unordered_map<int64_t, HotCluster> g_waterClusters;
	int g_nextWaterAt = 0;
	struct WaterFlow
	{
		float x, y; // the way it flows (unit)
		bool car;
	};
	std::vector<std::pair<Entity, WaterFlow>> g_waterCarried; // cars and loose things in flowing water (water_tick)
	// Screen effects, done on the finished picture so GTA and Minecraft move as one (screen_fx_tick): a camera shake
	// (GTA's own camera shake is stopped every frame: Minecraft's frame can't follow it, it lags a frame behind) and
	// the nether portal's warp while Steve stands in a portal.
	struct ScreenFx
	{
		float shake = 0.0f;       // current shake strength (decays)
		float rumble = 0.0f;      // a floor for it, while rumbling
		int rumbleUntil = 0;
		float warp = 0.0f, warpTarget = 0.0f;
		int warpPulseUntil = 0;
		int64_t last = 0;
		float t = 0.0f;
		// the picture shakes (GTA and Minecraft as one), and GTA's own camera shake is stopped and turned into that
		bool screenShake = true, suppress = true;
		int gtaShakeFrames = 0;                  // frames GTA's own camera shake was found (and stopped)
	} g_fx;
	Cam g_aimCam = 0; // while aiming: GTA's aim camera moved out past Steve's (much wider) head
	// The gun Steve visibly holds: GTA puts its own where its ped's hands are, inside Steve's blocky arms, so a
	// second copy of the weapon model sits in his hands (Minecraft's crossbow-hold pose), pointing where he aims.
	struct GunFit
	{
		Object held = 0;
		Hash heldHash = 0;
		// grip from Steve's feet: shouldered at his right cheek, so it shows past his head over the shoulder;
		// weapon models point along +x (yaw 90 turns that forward) and tilt about y
		float fwd = 0.30f, right = 0.30f, up = 1.42f, yaw = 90.0f, pitch = 0.0f, roll = 0.0f, tilt = -1.0f;
	} g_gunFit;
	constexpr double kMaxMinecraftPixels = 1920.0 * 1080.0;

	HMODULE g_module = nullptr;

	/// One line in MCPassthrough.log next to the plugin (started afresh each game, at most ~2 MB): what was done with
	/// leads, Minecraft's ground and Minecraft's player, so a report of something not working says what happened.
	void logf(const char *fmt, ...)
	{
		static FILE *file = nullptr;
		static bool opened = false;
		static long written = 0;
		if (!opened)
		{
			opened = true;
			char path[MAX_PATH] = {};
			const DWORD n = GetModuleFileNameA(g_module, path, MAX_PATH);
			char *dot = n > 0 ? std::strrchr(path, '.') : nullptr;
			if (dot != nullptr && dot + 5 <= path + MAX_PATH)
			{
				std::memcpy(dot, ".log", 5);
				if (fopen_s(&file, path, "w") != 0)
					file = nullptr;
			}
		}
		if (file == nullptr || written > 2000000)
			return;
		char line[400];
		va_list args;
		va_start(args, fmt);
		const int len = std::vsnprintf(line, sizeof(line), fmt, args);
		va_end(args);
		if (len <= 0)
			return;
		static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return double(f.QuadPart); }();
		LARGE_INTEGER c;
		QueryPerformanceCounter(&c);
		written += std::fprintf(file, "%10.3f %s\n", double(c.QuadPart) / freq, line);
		std::fflush(file);
	}

	// What tick() is doing now: named in MCPassthrough.log if something fails inside GTA meanwhile (script_main)
	const char *g_doing = "starting";
	char g_doingMessage[64] = "";

	WsClient g_ws;
	bool g_started = false;
	bool g_enabled = true;
	Ped g_hiddenPed = 0; // the player ped we hid (Steve stands in for it)
	int g_generation = -1;
	int g_viewSent = 0;

	// Minecraft-driven flight: Minecraft's physics (elytra, fireworks) move the player; GTA follows with a chase cam.
	struct Drive
	{
		bool on = false;
		Cam cam = 0;
		Vector3 pos = {}, vel = {};     // Minecraft's player (feet), GTA coordinates / velocity per second
		int64_t posNanos = 0;           // when Minecraft sampled it (its System.nanoTime = this process's QPC clock)
		bool havePos = false;
		float estX = 0, estY = 0, estZ = 0; // where Steve is drawn: carried by the velocity each frame, eased onto
		bool haveEst = false;               // Minecraft's samples (snapping to each made the flight stutter)
		float fov = 50.0f;
		float heading = 0, pitch = 0;   // where Steve looks (steers): director, or the mouse (gameplay cam)
		float sHeading = 0, sPitch = 0; // the same, smoothed per frame (steering updates arrive unevenly)
		int lookUntil = 0;
		float camX = 0, camY = 0, camZ = 0;
		bool camInit = false;
		float follow = 21.0f;           // chase-cam smoothing rate (1/s); starts low so the switch from GTA's camera is a blend
		int64_t lastTick = 0;
		// this frame's chase cam and the Steve position it frames (Minecraft draws Steve exactly there)
		float outX = 0, outY = 0, outZ = 0, outPitch = 0, outHeading = 0, outFov = 60.0f;
		float steveX = 0, steveY = 0, steveZ = 0;
		bool haveOut = false;
		float dist = 5.5f, height = 1.4f;
		// armed: start flying (elytra, launched along heading/pitch) as soon as the player drops off an edge
		bool armed = false;
		float armZ = 0, armHeading = 0, armPitch = 0, armSpeed = 1.0f, armDrop = 0.6f;
		int armHold = 3000;             // ms the arm heading/pitch steer before the director or the mouse does
		// the player flies it (an elytra worn, Space twice): the mouse steers, and touching down lands (back to walking)
		bool user = false;
		int launchedAt = 0, armAfter = 0;
	} g_drive;
	float g_yOffset = 0.0f;
	bool g_haveOffset = false;
	float g_levelX = 0.0f, g_levelY = 0.0f; // where the ground was last levelled (GTA coordinates)
	std::unordered_map<int64_t, float> g_sampled; // column -> the feet height it was probed for

	// Minecraft blocks mirrored as GTA props: solid for peds and cars, and (visible, a bit smaller than the
	// block so Minecraft always covers them) casting GTA shadows.
	struct BlockProps
	{
		std::string model = "prop_box_wood01a"; // 0.97 x 0.96 x 0.80 m: just inside a block
		bool visible = false; // visible props peek out of Minecraft's blocks during camera moves
		int alpha = 255;      // visible but faded: maybe shadows without the box showing
		Hash hash = 0;
		Vector3 mn = {}, mx = {};
		std::set<std::tuple<int, int, int>> blocks;            // Minecraft's solid blocks as last reported
		std::map<std::tuple<int, int, int>, Object> live;
		std::unordered_set<Object> handles;                    // the live props (GTA's probes skip them: Minecraft has the blocks)
		std::map<std::tuple<int, int, int>, int> turns;        // the way each live prop was turned (prop_turn)
		std::vector<std::tuple<int, int, int>> pending;
		// props without collision yet: something of GTA's was there when the block was placed (a solid prop put inside a
		// car or a person flings it far away: it looked as if the block despawned it)
		std::set<std::tuple<int, int, int>> ghosts;
		int nextGhostAt = 0;
		static constexpr size_t kMax = 400; // GTA crashes (access violation) with ~1500 script objects about
		static constexpr float kRadius = 100.0f; // blocks within this of the player get one (nearest first; 30 m: cars
		                                         // drove through what was built a street away)
		int nextPickAt = 0;
	} g_props;
	void props_clear_all();
	void world_mobs_message(const std::string &m);
	void leash_use(Ped player, const std::string &m);
	float entity_mass(Entity e);
	constexpr Hash kWaterMaterial = 0x19F81600; // GTA's water (MaterialHash.Water)
	constexpr Hash kPuddleMaterial = 0x3B982E13; // a puddle on the street
	Vector3 v3(float x, float y, float z);
	bool world_probe(Entity ignore, float x0, float y0, float z0, float x1, float y1, float z1, int flags, int options, Vector3 &at,
		Vector3 &normal, Entity &entity, Hash &material);
	bool solid_probe(Ped ped, float x0, float y0, float z0, float x1, float y1, float z1, Vector3 &at, Vector3 &normal);
	void mc_hit(Entity e, bool shot);
	float dist3(const Vector3 &a, const Vector3 &b);
	void leash_gone(int id);
	void boats_message(const std::string &m);
	void animal_message(const std::string &m);
	void command_message(Ped player, const std::string &m);
	void enderthief_message(Ped player, const std::string &m);

	// ---- GTA's own objects moved: the plugin's copies ----
	// The map's own objects (signs, bins, lamp posts, benches, doors) can't be woken, freed or pushed by a script: GTA
	// crashed doing it ("GTA failed during Minecraft's melee"). One knocked loose, tied to a lead, hooked or burnt is
	// swapped for the plugin's own copy (the map's hidden meanwhile), which moves as anything does. Put back as it was
	// when the player is far off, and by /clearall.
	struct LooseCopy
	{
		Object copy;   // 0: gone (burnt away, mined): the map's own stays hidden till the player is far off
		Hash model;
		float x, y, z; // where the map's own stands (hidden)
	};
	std::vector<LooseCopy> g_loose;
	constexpr size_t kMaxLoose = 48;

	bool is_loose(Entity e)
	{
		for (const LooseCopy &c : g_loose)
			if (c.copy != 0 && Entity(c.copy) == e)
				return true;
		return false;
	}

	/// One of the plugin's copies deleted by the plugin itself (burnt away, mined): the map's own isn't back till the
	/// player is far off.
	void loose_gone(Entity e)
	{
		for (LooseCopy &c : g_loose)
			if (c.copy != 0 && Entity(c.copy) == e)
				c.copy = 0;
	}

	void loose_restore(LooseCopy &c)
	{
		natives::RemoveModelHide(c.x, c.y, c.z, 0.3f, c.model);
		if (c.copy != 0 && natives::DoesEntityExist(c.copy))
			natives::DeleteObject(&c.copy);
		c.copy = 0;
	}

	/// What can be moved for GTA object `e`: the plugin's copy in place of one of the map's own (made now if need be), a
	/// script's own object itself, or 0 for nothing to move (not an object, carried, a pickup, one of Minecraft's blocks,
	/// Steve's gun, heavier than `maxKg`).
	Object loosen(Entity e, float maxKg = 1e9f)
	{
		if (e == 0 || !natives::DoesEntityExist(e) || natives::GetEntityType(e) != 3)
			return 0;
		if (is_loose(e))
			return Object(e);
		if (g_props.handles.count(e) || Object(e) == g_gunFit.held || natives::IsEntityAttached(e) || natives::IsObjectAPickup(Object(e)) ||
			natives::IsObjectAPortablePickup(Object(e)))
			return 0;
		if (natives::IsEntityAMissionEntity(e))
			return Object(e); // (a script's own: made to be moved)
		const Hash model = natives::GetEntityModel(e);
		if (model == 0 || !natives::IsModelValid(model) || entity_mass(e) > maxKg)
			return 0;
		// the map's own already swapped (hit again before GTA hid it, or found again by another probe): its copy, never a
		// second one (two of it stood there)
		{
			const Vector3 o = natives::GetEntityCoords(e, FALSE);
			for (const LooseCopy &c : g_loose)
				if (c.model == model && (c.x - o.x) * (c.x - o.x) + (c.y - o.y) * (c.y - o.y) + (c.z - o.z) * (c.z - o.z) < 0.5f * 0.5f)
					return c.copy != 0 && natives::DoesEntityExist(c.copy) ? c.copy : 0;
		}
		if (!natives::HasModelLoaded(model))
		{
			natives::RequestModel(model); // (on screen it's loaded; else next time)
			return 0;
		}
		if (g_loose.size() >= kMaxLoose)
		{
			// the oldest put back first (GTA's pool of script objects is small)
			loose_restore(g_loose.front());
			g_loose.erase(g_loose.begin());
		}
		const Vector3 at = natives::GetEntityCoords(e, FALSE), rot = natives::GetEntityRotation(e);
		const Object copy = natives::CreateDynamicObject(model, at.x, at.y, at.z);
		if (copy == 0 || !natives::DoesEntityExist(copy))
			return 0;
		natives::SetEntityRotation(copy, rot.x, rot.y, rot.z);
		natives::CreateModelHideExcludingScriptObjects(at.x, at.y, at.z, 0.3f, model);
		g_loose.push_back({copy, model, at.x, at.y, at.z});
		logf("loose: map object %d (model %08X) at %.1f %.1f %.1f swapped for copy %d", int(e), unsigned(model), at.x, at.y, at.z, int(copy));
		return copy;
	}

	/// Whether GTA object `e` may be pushed as it is (the plugin's copy, or a script's own: not one of the map's).
	bool movable_object(Entity e)
	{
		return e != 0 && (is_loose(e) || (natives::IsEntityAMissionEntity(e) && !g_props.handles.count(e)));
	}

	/// Now and then: copies far from the player put back, the map's own showing again where it stood.
	void loose_tick(Ped player)
	{
		static int next = 0;
		const int now = natives::GetGameTimer();
		if (g_loose.empty() || now < next)
			return;
		next = now + 1000;
		const Vector3 me = natives::GetEntityCoords(player, TRUE);
		for (size_t i = 0; i < g_loose.size();)
		{
			LooseCopy &c = g_loose[i];
			if (c.copy != 0 && !natives::DoesEntityExist(c.copy))
				c.copy = 0; // (gone: the map's own isn't back till the player is far off)
			const float dx = c.x - me.x, dy = c.y - me.y;
			if (dx * dx + dy * dy < 150.0f * 150.0f)
			{
				++i;
				continue;
			}
			loose_restore(c);
			g_loose.erase(g_loose.begin() + i);
		}
	}

	void loose_clear()
	{
		for (LooseCopy &c : g_loose)
			loose_restore(c);
		g_loose.clear();
	}

	// Things waiting on GTA (a model still loading): tried again each frame for up to 5 s, never waited for inside a
	// frame (that froze everything: no camera for Minecraft, no walking)
	std::vector<std::pair<int, std::function<void()>>> g_later;
	int g_laterDeadline = 0;

	void later(std::function<void()> retry)
	{
		g_later.push_back({g_laterDeadline != 0 ? g_laterDeadline : natives::GetGameTimer() + 5000, std::move(retry)});
	}

	/// `then` at game time `at` (within the 5 s anything waits on GTA)
	void after(int at, std::function<void()> then)
	{
		later([at, then]() {
			if (natives::GetGameTimer() < at)
				after(at, then);
			else
				then();
		});
	}

	void later_tick()
	{
		if (g_later.empty())
			return;
		const int now = natives::GetGameTimer();
		auto due = std::move(g_later);
		g_later.clear();
		for (auto &[deadline, retry] : due)
			if (now < deadline)
			{
				g_laterDeadline = deadline;
				retry();
			}
		g_laterDeadline = 0;
	}

	/// Knocked flying: down (ragdoll) and pushed, the push given again once GTA has the ragdoll going (given the same
	/// frame the ragdoll starts, it was often lost), and the hurt only after that: killed first, they dropped dead where
	/// they stood instead of flying.
	struct Knock
	{
		Ped q;
		float x, y, z;
		int damage, start;
		bool pushed;
	};
	void knock_step(Knock k)
	{
		if (!natives::DoesEntityExist(k.q))
			return;
		const int now = natives::GetGameTimer();
		if (!k.pushed && natives::IsPedRagdoll(k.q))
		{
			natives::ApplyForceToEntity(k.q, k.x, k.y, k.z);
			k.pushed = true;
		}
		if (now - k.start < 180)
		{
			later([k]() { knock_step(k); });
			return;
		}
		if (k.damage > 0)
			natives::ApplyDamageToPed(k.q, k.damage);
	}
	void knock_ped(Ped q, float x, float y, float z, int ms, int damage)
	{
		natives::SetPedToRagdoll(q, ms);
		natives::ApplyForceToEntity(q, x, y, z);
		knock_step(Knock{q, x, y, z, damage, natives::GetGameTimer(), false});
	}

	/// A thing of GTA's (a sign, a bin, a lamp post) knocked loose and sent off at (x, y, z) m/s: the map's own swapped
	/// for the plugin's copy first (its model loaded first if it isn't: the hit isn't lost), and the push given again
	/// for a few frames until the copy moves (a copy made this frame has no physics yet: a push then did nothing).
	struct Launch
	{
		Entity e;
		Object ob;
		float x, y, z, maxKg;
		int start;
	};
	void launch_step(Launch l)
	{
		const int now = natives::GetGameTimer();
		if (l.ob == 0)
		{
			if (!natives::DoesEntityExist(l.e))
				return;
			l.ob = loosen(l.e, l.maxKg);
			if (l.ob == 0)
			{
				const Hash model = natives::GetEntityModel(l.e);
				// (only waiting on its model: anything else that stops it won't change)
				if (now - l.start < 1500 && model != 0 && natives::IsModelValid(model) && !natives::HasModelLoaded(model))
					later([l]() { launch_step(l); });
				return;
			}
		}
		if (!natives::DoesEntityExist(l.ob))
			return;
		natives::FreezeEntityPosition(l.ob, FALSE);
		natives::SetEntityDynamic(l.ob, TRUE);
		natives::ActivatePhysics(l.ob);
		const float want = std::sqrt(l.x * l.x + l.y * l.y + l.z * l.z);
		if (natives::GetEntitySpeed(l.ob) > 0.4f * want)
			return; // off it goes
		natives::SetEntityVelocity(l.ob, l.x, l.y, l.z);
		natives::ApplyForceToEntity(l.ob, l.x, l.y, l.z); // (a lamp post or sign stands fixed till broken off its base)
		if (now - l.start < 600)
			later([l]() { launch_step(l); });
	}
	void launch_object(Entity e, float x, float y, float z, float maxKg)
	{
		launch_step(Launch{e, 0, x, y, z, maxKg, natives::GetGameTimer()});
	}

	// /gta's toggles: GTA's cheats, kept on every frame (cheats_tick)
	struct Cheats
	{
		bool superJump = false, fastRun = false, fastSwim = false, explosiveAmmo = false, fireAmmo = false, explosiveMelee = false,
			slidey = false, moon = false, slowmo = false, infiniteAmmo = false, neverWanted = false, drunk = false, oneHit = false,
			blackout = false, frozenTime = false;
		float traffic = -1.0f, crowds = -1.0f; // multipliers, -1: GTA's own
		bool haveHome = false;
		Vector3 home = {};
	} g_cheats;
	float g_stamina = 100.0f;
	bool g_exhausted = false;
	int g_staminaRestAt = 0;
	bool g_mcScoping = false;  // Minecraft's player looks through a spyglass (the camera zooms in)
	int g_ctrlZoomAt = -1000;  // when Ctrl's zoom was last held (meanwhile the mouse wheel zooms, not the hotbar)
	bool g_mapLocked = false;  // the minimap turned by our camera (GTA's turns by its own only)
	bool g_mcTotem = false;    // Minecraft's player holds a totem of undying (it saves GTA's player from dying)
	// While a totem is held, GTA's player has this much hidden health on top (a one-hit death, a blast or a sniper,
	// takes it below its real health, and the totem saves it there): its real health is GTA's less this
	int g_totemBuffer = 0;
	Ped g_totemPed = 0;
	constexpr int kTotemBuffer = 2000;

	int real_health(Ped p)
	{
		return natives::GetEntityHealth(p) - (p == g_totemPed ? g_totemBuffer : 0);
	}

	int real_max_health(Ped p)
	{
		return natives::GetEntityMaxHealth(p) - (p == g_totemPed ? g_totemBuffer : 0);
	}

	/// The hidden health off again (the totem used, put away, or god mode): GTA's player as it really is.
	void totem_unbuffer()
	{
		if (g_totemBuffer == 0)
			return;
		if (natives::DoesEntityExist(g_totemPed))
		{
			const int hp = natives::GetEntityHealth(g_totemPed) - g_totemBuffer;
			natives::SetEntityMaxHealth(g_totemPed, natives::GetEntityMaxHealth(g_totemPed) - g_totemBuffer);
			if (!natives::IsEntityDead(g_totemPed))
				natives::SetEntityHealth(g_totemPed, std::max(101, hp));
		}
		g_totemBuffer = 0;
		g_totemPed = 0;
	}
	bool g_mcBlocking = false; // Minecraft's player holds up a shield (bullets and blows from the front don't hurt)
	bool g_proofsDirty = false; // GTA's player's proofs were reset (F9 off): the shield's and fire resistance's again
	std::string g_mcEffects; // Minecraft's effects on its player: "speed:1,jump_boost:0,..." (effect:amplifier)
	void summon(Ped player, std::string what);
	void dent_vehicle(Vehicle v, const Vector3 &at, float damage, float radius);
	bool g_bowDrawn = false; // a bow drawn or a loaded crossbow in hand (Minecraft says: "bowdraw")
	int g_bowAt = 0;
	std::vector<std::pair<int, int>> g_spiral;
	std::atomic<bool> g_toggle{false};
	std::atomic<bool> g_relevel{false};

	// Controls GTA must not act on while Minecraft owns the mouse: attacking/aiming, melee, weapon selection.
	const int kDisabledControls[] = {
		24, 25, 257, 140, 141, 142, 143, 263, 264,       // attack, aim, attack 2, melee
		14, 15, 16, 17, 37, 261, 262,                    // weapon wheel / next / previous
		157, 158, 159, 160, 161, 162, 163, 164, 165,     // weapon slots (number keys)
		44, 45, 47, 58,                                  // cover, reload, detonate, throw grenade
		68, 69, 70, 91, 92, 99, 100, 114, 115, 116,      // vehicle / passenger weapons
	};
	// Number keys 1..9 are GTA's weapon-slot controls in this order.
	const int kHotbarControls[9] = {157, 158, 160, 164, 165, 159, 161, 162, 163};

	int64_t column_key(int x, int z)
	{
		return (int64_t(x) << 32) ^ uint32_t(z);
	}

	float wrap_degrees(float a)
	{
		a = std::fmod(a, 360.0f);
		if (a > 180.0f)
			a -= 360.0f;
		if (a <= -180.0f)
			a += 360.0f;
		return a;
	}

	void sendf(const char *format, ...)
	{
		char buffer[8192];
		va_list args, again;
		va_start(args, format);
		va_copy(again, args);
		const int len = vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);
		if (len >= int(sizeof(buffer)))
		{
			// (a long one whole, not cut off into broken JSON)
			std::string big(size_t(len) + 1, '\0');
			vsnprintf(big.data(), big.size(), format, again);
			big.resize(size_t(len));
			g_ws.send(big);
		}
		else if (len > 0)
			g_ws.send(buffer);
		va_end(again);
	}

	/// Steve stands in for GTA's player: hide it (again, if a mission showed it), or give back the one we hid. (Hidden
	/// for real: only not drawn a frame at a time, GTA still drew the character's head in first person, in the way of
	/// everything, and showed it through Steve.)
	void hide_player(Ped ped)
	{
		if (g_hiddenPed != 0 && g_hiddenPed != ped && natives::DoesEntityExist(g_hiddenPed))
			natives::SetEntityVisible(g_hiddenPed, TRUE, FALSE); // the character switched away from shows again
		if (natives::IsEntityVisible(ped))
			natives::SetEntityVisible(ped, FALSE, FALSE);
		natives::SetEntityLocallyInvisible(ped);
		g_hiddenPed = ped;
	}

	void unhide_player()
	{
		if (g_hiddenPed != 0 && natives::DoesEntityExist(g_hiddenPed))
			natives::SetEntityVisible(g_hiddenPed, TRUE, FALSE);
		g_hiddenPed = 0;
	}

	/// GTA's water surface over (x, y) between `low` (its bed, or the feet) and `high`, if there's water there. (GET_WATER_HEIGHT
	/// turns down points well above the water, and knows nothing of rivers: asked at a few heights, then GTA's probe
	/// against all its water, rivers too.)
	bool water_surface(float x, float y, float low, float high, float &surface)
	{
		for (const float z : {low + 0.5f, low + 3.0f, (low + high) * 0.5f, high})
			if (natives::GetWaterHeight(x, y, z, &surface) && surface > low)
				return true;
		float h = 0.0f;
		if (natives::TestProbeAgainstAllWater(x, y, high, x, y, low, 1, &h) == 1 && h > low && h < high + 5.0f)
		{
			surface = h;
			return true;
		}
		return false;
	}

	/// Find the ground under columns around the player that haven't been sampled yet (or only from another floor of
	/// a building), nearest first. Probes start a little above the feet: under any ceiling, so a hallway's ceiling is
	/// never taken for ground (which put barriers at head height). Under GTA's water the ground is its bed, and the
	/// water over it goes to Minecraft too (as water nobody sees: Minecraft swims in it). Where something of GTA's stands
	/// on the ground up to a mob's height (a wall, a fence, a parked bus stop), the column goes up to its top, so
	/// Minecraft's mobs bump into GTA's walls (the player Minecraft moves has GTA's real collision instead).
	void sample_ground(const Vector3 &player)
	{
		if (g_spiral.empty())
		{
			for (int dx = -kGroundRadius; dx <= kGroundRadius; ++dx)
				for (int dz = -kGroundRadius; dz <= kGroundRadius; ++dz)
					if (dx * dx + dz * dz <= kGroundRadius * kGroundRadius)
						g_spiral.emplace_back(dx, dz);
			std::sort(g_spiral.begin(), g_spiral.end(), [](auto &a, auto &b) {
				return a.first * a.first + a.second * a.second < b.first * b.first + b.second * b.second;
			});
		}
		const int px = int(std::floor(player.x)), pz = int(std::floor(-player.y));
		const float feet = player.z - 1.0f;
		{
			// columns far behind forgotten now and then (probed again on the way back): it grew for ever
			static int nextPrune = 0;
			const int now = natives::GetGameTimer();
			if (now >= nextPrune)
			{
				nextPrune = now + 10000;
				for (auto it = g_sampled.begin(); it != g_sampled.end();)
				{
					const int x = int(it->first >> 32), z = int(uint32_t(it->first));
					it = std::abs(x - px) > 200 || std::abs(z - pz) > 200 ? g_sampled.erase(it) : std::next(it);
				}
			}
		}
		std::string columns, water;
		int probes = 0;
		const Ped ped = natives::PlayerPedId();
		for (const auto &[dx, dz] : g_spiral)
		{
			const int x = px + dx, z = pz + dz;
			const auto done = g_sampled.find(column_key(x, z));
			if (done != g_sampled.end() && std::fabs(done->second - feet) < 2.0f)
				continue;
			if (++probes > kGroundProbesPerTick)
				break;
			float groundZ = 0.0f;
			// Minecraft column (x, z) covers GTA x in [x, x+1) and y in (-z-1, -z]: probe its centre
			const float cx = x + 0.5f, cy = -(z + 0.5f);
			if (!natives::GetGroundZFor3dCoord(cx, cy, feet + 1.2f, &groundZ, TRUE, FALSE) || groundZ == 0.0f)
			{
				// no seabed found under deep water (the open sea, a deep dock: GTA hasn't its bottom loaded): still water to
				// Minecraft, over a bed well down (Minecraft swims and its trident's riptide works there too)
				float sea = 0.0f;
				if (!water_surface(cx, cy, feet - 6.0f, feet + 20.0f, sea))
					continue; // collision not streamed in yet: try again later
				groundZ = std::min(sea, feet) - 14.0f;
			}
			g_sampled[column_key(x, z)] = feet;
			const int top = int(std::floor(groundZ + g_yOffset + 0.5f)) - 1;
			// something standing on the ground here, up to a mob's height (not a car: they drive off): its top, or a
			// wall's face crossing the column (a probe starting inside a thick wall doesn't see it from above)
			int wallTop = top;
			{
				// (world_probe: shallow water's surface isn't something standing there: it put barriers on the water)
				Vector3 at = {}, n = {};
				Entity e = 0;
				Hash material = 0;
				if (world_probe(ped, cx, cy, groundZ + 1.8f, cx, cy, groundZ + 0.45f, 1 | 16, 7, at, n, e, material) && at.z > groundZ + 0.5f &&
					(e == 0 || !g_props.handles.count(e)))
					wallTop = std::max(top, int(std::floor(at.z + g_yOffset + 0.5f)) - 1);
				for (int axis = 0; axis < 2 && wallTop < top + 2; ++axis)
				{
					const float ax = axis == 0 ? 0.45f : 0.0f, ay = axis == 0 ? 0.0f : 0.45f;
					if (world_probe(ped, cx - ax, cy - ay, groundZ + 1.0f, cx + ax, cy + ay, groundZ + 1.0f, 1 | 16, 7, at, n, e, material) &&
						(e == 0 || !g_props.handles.count(e)))
						wallTop = top + 2;
				}
			}
			char entry[64];
			snprintf(entry, sizeof(entry), "%s%d,%d,%d,%d", columns.empty() ? "" : ",", x, z, top - kGroundDepth + 1, wallTop);
			columns += entry;
			float surface = 0.0f;
			if (water_surface(cx, cy, groundZ, std::max(feet, groundZ) + 20.0f, surface) && surface > groundZ + 0.1f)
			{
				snprintf(entry, sizeof(entry), "%s%d,%d,%d,%d", water.empty() ? "" : ",", x, z, top + 1, int((surface + g_yOffset) * 256.0f));
				water += entry;
			}
		}
		if (!columns.empty())
			g_ws.send("{\"t\":\"ground\",\"c\":[" + columns + "]}");
		if (!water.empty())
		{
			g_ws.send("{\"t\":\"gwater\",\"c\":[" + water + "]}");
			static int lastLog = -100000;
			if (natives::GetGameTimer() - lastLog > 5000)
			{
				lastLog = natives::GetGameTimer();
				logf("GTA's water to Minecraft: %s", water.substr(0, 120).c_str());
			}
		}
	}

	/// Past the JSON string starting at p (on its opening quote): just after its closing quote (escapes skipped).
	const char *json_skip_string(const char *p)
	{
		for (++p; *p != '\0' && *p != '"'; ++p)
			if (*p == '\\' && p[1] != '\0')
				++p;
		return *p == '"' ? p + 1 : p;
	}

	/// The value of `key` in a JSON message (the first one, at any depth), with or without spaces around ':'. Only
	/// real keys count: a key's name inside some string value (a chat line, a name) isn't one.
	const char *json_value(const std::string &m, const char *key)
	{
		const size_t keyLen = std::strlen(key);
		for (const char *p = m.c_str(); *p != '\0';)
		{
			if (*p != '"')
			{
				++p;
				continue;
			}
			const char *start = p + 1, *after = json_skip_string(p);
			const bool match = size_t(after - start) == keyLen + 1 && std::strncmp(start, key, keyLen) == 0;
			p = after;
			while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
				++p;
			if (*p != ':')
				continue; // (a string value, not a key)
			++p;
			while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
				++p;
			if (match)
				return p;
		}
		return nullptr;
	}

	double json_num(const std::string &m, const char *key, double fallback)
	{
		const char *p = json_value(m, key);
		return p ? std::atof(p) : fallback;
	}

	std::string json_str(const std::string &m, const char *key)
	{
		const char *p = json_value(m, key);
		if (p == nullptr || *p != '"')
			return {};
		// (escapes read: a quote or a backslash in it no longer cuts it off there)
		std::string out;
		for (++p; *p != '\0' && *p != '"'; ++p)
		{
			if (*p != '\\' || p[1] == '\0')
			{
				out += *p;
				continue;
			}
			++p;
			switch (*p)
			{
			case 'n': out += '\n'; break;
			case 't': out += '\t'; break;
			case 'r': out += '\r'; break;
			case 'b': out += '\b'; break;
			case 'f': out += '\f'; break;
			case 'u':
			{
				unsigned c = 0;
				int n = 0;
				for (; n < 4 && std::isxdigit(static_cast<unsigned char>(p[1])); ++n, ++p)
					c = c * 16 + unsigned(std::isdigit(static_cast<unsigned char>(*(p + 1))) ? *(p + 1) - '0' : (std::tolower(*(p + 1)) - 'a' + 10));
				if (c < 0x80)
					out += char(c);
				else if (c < 0x800)
				{
					out += char(0xC0 | (c >> 6));
					out += char(0x80 | (c & 0x3F));
				}
				else
				{
					out += char(0xE0 | (c >> 12));
					out += char(0x80 | ((c >> 6) & 0x3F));
					out += char(0x80 | (c & 0x3F));
				}
				break;
			}
			default: out += *p; break; // \" \\ \/
			}
		}
		return out;
	}

	/// Keep GTA's (invisible) player alive, unragdolled and unwanted through Minecraft's explosions.
	void make_safe(Ped ped)
	{
		const Player player = natives::PlayerId();
		natives::SetEntityInvincible(ped, TRUE);
		natives::SetPlayerInvincible(player, TRUE);
		natives::SetEntityProofs(ped, TRUE, TRUE, TRUE, TRUE, TRUE);
		natives::SetPedCanRagdoll(ped, FALSE);
		natives::SetMaxWantedLevel(0);
		natives::ClearPlayerWantedLevel(player);
		natives::SetPoliceIgnorePlayer(player, TRUE);
		natives::SetDispatchCopsForPlayer(player, FALSE);
	}

	/// God mode (F9) on: make_safe's rules. Off: GTA's own (the player can die, the police come), as story missions
	/// expect. Only ever applied when it changes, so whatever a mission sets for itself stays.
	void apply_rules(Ped ped)
	{
		if (g_godMode)
		{
			make_safe(ped);
			return;
		}
		const Player player = natives::PlayerId();
		natives::SetEntityInvincible(ped, FALSE);
		natives::SetPlayerInvincible(player, FALSE);
		natives::SetEntityProofs(ped, FALSE, FALSE, FALSE, FALSE, FALSE);
		g_proofsDirty = true; // (the shield's and fire resistance's go on again)
		natives::SetPedCanRagdoll(ped, TRUE);
		natives::SetMaxWantedLevel(5);
		natives::SetPoliceIgnorePlayer(player, FALSE);
		natives::SetDispatchCopsForPlayer(player, TRUE);
	}

	void send_state(Ped ped)
	{
		const Vector3 p = natives::GetEntityCoords(ped, TRUE);
		const Vector3 c = natives::GetFinalRenderedCamCoord();
		const Vector3 r = natives::GetFinalRenderedCamRot(2);
		sendf("{\"t\":\"gtastate\",\"frame\":%d,\"pos\":[%.3f,%.3f,%.3f],\"h\":%.2f,\"cam\":[%.3f,%.3f,%.3f],\"rot\":[%.2f,%.2f,%.2f],"
			  "\"fov\":%.2f,\"view\":%d,\"interior\":%d,\"yoff\":%.4f}",
			natives::GetFrameCount(), p.x, p.y, p.z, natives::GetEntityHeading(ped), c.x, c.y, c.z, r.x, r.y, r.z,
			natives::GetFinalRenderedCamFov(), natives::GetFollowPedCamViewMode(), natives::GetInteriorFromEntity(ped), g_yOffset);
	}

	/// Nanoseconds on the QueryPerformanceCounter clock, computed the way Java's System.nanoTime does on Windows,
	/// so Minecraft's timestamps compare directly.
	int64_t now_nanos()
	{
		static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return double(f.QuadPart); }();
		LARGE_INTEGER c;
		QueryPerformanceCounter(&c);
		return int64_t(double(c.QuadPart) / freq * 1e9);
	}

	/// A jolt of shake: `strength` 1 is a big explosion close by.
	void shake_impulse(float strength)
	{
		g_fx.shake = std::min(1.6f, std::max(g_fx.shake, 0.0f) + strength);
	}

	/// A jolt scaled by how far away something blew up (GTA coordinates).
	void shake_from(float x, float y, float z, float strength)
	{
		const Vector3 c = natives::GetFinalRenderedCamCoord();
		const float d = std::sqrt((x - c.x) * (x - c.x) + (y - c.y) * (y - c.y) + (z - c.z) * (z - c.z));
		shake_impulse(strength * std::clamp(1.25f - d / 45.0f, 0.0f, 1.0f));
	}


	/// Tab: GTA's own weapons (GTA takes the mouse to aim and fire, the wheel and number keys pick a weapon, and Steve
	/// holds it) or Minecraft's items (the mouse, wheel and number keys go to Minecraft).
	void hands_set(Ped ped, bool gta)
	{
		if (gta == g_gtaHands)
			return;
		g_gtaHands = gta;
		if (gta)
		{
			if (g_gtaWeapon != 0 && natives::HasPedGotWeapon(ped, g_gtaWeapon))
				natives::SetCurrentPedWeapon(ped, g_gtaWeapon, TRUE);
			natives::Notify("Hands: ~b~GTA weapons~s~ (mouse wheel or 1-9 to pick one, Tab for Minecraft)");
		}
		else
		{
			const Hash w = natives::GetSelectedPedWeapon(ped);
			if (w != kWeaponUnarmed)
				g_gtaWeapon = w;
			natives::SetCurrentPedWeapon(ped, kWeaponUnarmed, TRUE);
			natives::Notify("Hands: ~g~Minecraft items~s~ (Tab for GTA weapons)");
		}
	}

	/// The director's gun op: one of kGuns in GTA hands, or (-1) back to Minecraft's items.
	void gun_set(Ped ped, int gun)
	{
		if (gun < 0 || gun >= int(std::size(kGuns)))
		{
			hands_set(ped, false);
			return;
		}
		natives::GiveWeaponToPed(ped, kGuns[gun], 9999, FALSE, TRUE);
		natives::SetCurrentPedWeapon(ped, kGuns[gun], TRUE);
		natives::SetPedInfiniteAmmoClip(ped, TRUE);
		g_gtaWeapon = kGuns[gun];
		g_gtaHands = true;
	}

	/// Steve holds a GTA weapon (not his fists): Minecraft poses his arms for it.
	bool holding_gta_weapon(Ped ped)
	{
		return g_gtaHands && natives::GetSelectedPedWeapon(ped) != kWeaponUnarmed;
	}

	/// Keep the visible gun in Steve's hands: his arms point where the camera looks (Minecraft's crossbow hold
	/// follows the head), from shoulders at his height; the grip sits `fwd` out along that line.
	void gun_model_tick(Ped ped, bool show)
	{
		const Hash want = show && holding_gta_weapon(ped) ? natives::GetSelectedPedWeapon(ped) : 0;
		if (g_gunFit.held != 0 && g_gunFit.heldHash != want)
		{
			natives::DeleteObject(&g_gunFit.held);
			g_gunFit.held = 0;
		}
		if (want == 0)
			return;
		const Vector3 p = natives::GetEntityCoords(ped, TRUE);
		if (g_gunFit.held == 0)
		{
			natives::RequestWeaponAsset(want);
			if (!natives::HasWeaponAssetLoaded(want))
				return;
			g_gunFit.held = natives::CreateWeaponObject(want, p.x, p.y, p.z);
			g_gunFit.heldHash = want;
			natives::SetEntityCollision(g_gunFit.held, FALSE, FALSE);
		}
		const Vector3 r = natives::GetGameplayCamRot(2);
		const float d2r = 3.14159265f / 180.0f, h = r.z * d2r, pt = r.x * d2r;
		const float fx = -std::sin(h) * std::cos(pt), fy = std::cos(h) * std::cos(pt), fz = std::sin(pt);
		const float rx = std::cos(h), ry = std::sin(h);
		const float feetZ = p.z - 1.0f;
		const float x = p.x + rx * g_gunFit.right + fx * g_gunFit.fwd;
		const float y = p.y + ry * g_gunFit.right + fy * g_gunFit.fwd;
		const float z = feetZ + g_gunFit.up + fz * g_gunFit.fwd;
		natives::SetEntityCoordsNoOffset(g_gunFit.held, x, y, z);
		natives::SetEntityRotation(g_gunFit.held, g_gunFit.pitch, g_gunFit.roll + r.x * g_gunFit.tilt, r.z + g_gunFit.yaw);
	}

	/// The crosshair: where GTA's aim (its own camera's centre ray) meets the world, as seen from the camera that
	/// renders (the aim camera is moved out past Steve's head, so that isn't the middle of the screen).
	void reticle_tick(Ped ped)
	{
		const Vector3 c = natives::GetGameplayCamCoord();
		const Vector3 r = natives::GetGameplayCamRot(2);
		const float d2r = 3.14159265f / 180.0f, h = r.z * d2r, pt = r.x * d2r;
		const float fx = -std::sin(h) * std::cos(pt), fy = std::cos(h) * std::cos(pt), fz = std::sin(pt);
		float tx = c.x + fx * 150.0f, ty = c.y + fy * 150.0f, tz = c.z + fz * 150.0f;
		const int probe = natives::StartShapeTestLosProbe(c.x + fx * 1.5f, c.y + fy * 1.5f, c.z + fz * 1.5f, tx, ty, tz, -1, ped);
		BOOL hit = FALSE;
		Vector3 end = {}, normal = {};
		Entity entity = 0;
		if (natives::GetShapeTestResult(probe, &hit, &end, &normal, &entity) == 2 && hit)
		{
			tx = end.x; ty = end.y; tz = end.z;
		}
		float sx = 0.5f, sy = 0.5f;
		if (!natives::GetScreenCoordFromWorldCoord(tx, ty, tz, &sx, &sy))
			return;
		natives::DrawRect(sx, sy, 0.011f, 0.0021f, 255, 255, 255, 220);
		natives::DrawRect(sx, sy, 0.0012f, 0.019f, 255, 255, 255, 220);
	}

	/// GTA's over-the-shoulder aim camera is framed for GTA's ped, whose head is far narrower than Steve's: while
	/// aiming, render from the same camera moved out to the right and back, so Steve's head clears the reticle.
	void aim_cam_tick(bool aiming)
	{
		if (!aiming)
		{
			if (g_aimCam != 0)
			{
				natives::RenderScriptCams(FALSE, TRUE, 200);
				natives::DestroyCam(g_aimCam);
				g_aimCam = 0;
			}
			return;
		}
		const Vector3 c = natives::GetGameplayCamCoord();
		const Vector3 r = natives::GetGameplayCamRot(2);
		const float d2r = 3.14159265f / 180.0f, h = r.z * d2r, pt = r.x * d2r;
		const float fx = -std::sin(h) * std::cos(pt), fy = std::cos(h) * std::cos(pt), fz = std::sin(pt);
		const float rx = std::cos(h), ry = std::sin(h);
		const float x = c.x + rx * 0.45f - fx * 0.35f, y = c.y + ry * 0.45f - fy * 0.35f, z = c.z - fz * 0.35f + 0.05f;
		if (g_aimCam == 0)
		{
			g_aimCam = natives::CreateCam("DEFAULT_SCRIPTED_CAMERA");
			natives::SetCamCoord(g_aimCam, x, y, z);
			natives::SetCamRot(g_aimCam, r.x, r.y, r.z);
			natives::SetCamFov(g_aimCam, natives::GetGameplayCamFov());
			natives::SetCamActive(g_aimCam, TRUE);
			natives::RenderScriptCams(TRUE, TRUE, 200);
		}
		natives::SetCamCoord(g_aimCam, x, y, z);
		natives::SetCamRot(g_aimCam, r.x, r.y, r.z);
		natives::SetCamFov(g_aimCam, natives::GetGameplayCamFov());
	}

	/// Minecraft-driven flight on/off. On: a scripted chase camera takes over from GTA's, starting where GTA's camera
	/// is and blending in; GTA's player is frozen and rides along with Minecraft's.
	void drive_set(Ped ped, bool want, bool ease = false)
	{
		if (want && !g_drive.on)
		{
			const Vector3 c = natives::GetFinalRenderedCamCoord();
			const Vector3 r = natives::GetFinalRenderedCamRot(2);
			g_drive.cam = natives::CreateCam("DEFAULT_SCRIPTED_CAMERA");
			g_drive.haveEst = false;
			{
				const float f = natives::GetFinalRenderedCamFov();
				g_drive.fov = f >= 30.0f && f <= 80.0f ? f : 50.0f; // GTA's, kept for the whole flight
			}
			natives::SetCamCoord(g_drive.cam, c.x, c.y, c.z);
			natives::SetCamRot(g_drive.cam, r.x, r.y, r.z);
			natives::SetCamFov(g_drive.cam, natives::GetFinalRenderedCamFov());
			natives::SetCamActive(g_drive.cam, TRUE);
			natives::RenderScriptCams(TRUE);
			natives::FreezeEntityPosition(ped, TRUE);
			natives::SetEntityCollision(ped, FALSE, FALSE);
			g_drive.heading = g_drive.sHeading = natives::GetGameplayCamRot(2).z;
			g_drive.pitch = g_drive.sPitch = 0;
			g_drive.camX = c.x; g_drive.camY = c.y; g_drive.camZ = c.z;
			g_drive.camInit = true;
			g_drive.follow = 4.0f;
			g_drive.lastTick = now_nanos();
			g_drive.havePos = false;
			g_drive.haveOut = false;
		}
		else if (!want && g_drive.on)
		{
			natives::RenderScriptCams(FALSE, ease, 700);
			natives::DestroyCam(g_drive.cam);
			g_drive.cam = 0;
			natives::FreezeEntityPosition(ped, FALSE);
			natives::SetEntityCollision(ped, TRUE, TRUE);
		}
		g_drive.on = want;
		g_drive.armed = false;
	}

	constexpr Hash kPistol = 0x1B06D571, kSniper = 0x05FC3C11;

	// Probes that see glass: map, vehicles, objects and glass panes, skipping only see-through (chain-link) and
	// collision-less things. (The default options skip glass, so nothing ever "hit" a window.)
	constexpr int kGlassFlags = 1 | 2 | 16 | 64, kSeeGlass = 6;

	/// Glass: windows (shoot-through, frosted, lit, bulletproof), perspex, car windows of every kind.
	bool breakable_glass(Hash material)
	{
		switch (material)
		{
		case 0x37E12A0B: // GLASS_SHOOT_THROUGH
		case 0x0E931A0E: // GLASS_BULLETPROOF
		case 0x596C55D1: // GLASS_OPAQUE
		case 0x5978A2ED: // EMISSIVE_GLASS
		case 0x9F73E76C: // PERSPEX
		case 0x4A57FFCA: // CAR_GLASS_WEAK
		case 0x23EF48BC: // CAR_GLASS_MEDIUM
		case 0x3FD6150A: // CAR_GLASS_STRONG
		case 0x995DA5E6: // CAR_GLASS_BULLETPROOF
		case 0x1E94B2B7: // CAR_GLASS_OPAQUE
			return true;
		default:
			return false;
		}
	}

	/// The window of vehicle `v` nearest to (x, y, z) within `reach`, as SMASH_VEHICLE_WINDOW's index, or -1.
	int nearest_window(Vehicle v, float x, float y, float z, float reach)
	{
		static const struct
		{
			const char *bone;
			int index;
		} kWindows[] = {{"window_lf", 0}, {"window_rf", 1}, {"window_lr", 2}, {"window_rr", 3}, {"window_lm", 4}, {"window_rm", 5},
			{"windscreen", 6}, {"windscreen_r", 7}};
		int best = -1;
		float bestD = reach * reach;
		for (const auto &w : kWindows)
		{
			const int bone = natives::GetEntityBoneIndexByName(v, w.bone);
			if (bone < 0)
				continue;
			const Vector3 b = natives::GetWorldPositionOfEntityBone(v, bone);
			const float d = (b.x - x) * (b.x - x) + (b.y - y) * (b.y - y) + (b.z - z) * (b.z - z);
			if (d < bestD)
			{
				bestD = d;
				best = w.index;
			}
		}
		return best;
	}

	/// The tyre of vehicle `v` whose wheel is within `reach` of (x, y, z), as SET_VEHICLE_TYRE_BURST's index, or -1.
	int nearest_tyre(Vehicle v, float x, float y, float z, float reach)
	{
		static const struct
		{
			const char *bone;
			int index;
		} kWheels[] = {{"wheel_lf", 0}, {"wheel_rf", 1}, {"wheel_lm1", 2}, {"wheel_rm1", 3}, {"wheel_lr", 4}, {"wheel_rr", 5},
			{"wheel_lm2", 45}, {"wheel_rm2", 47}};
		int best = -1;
		float bestD = reach * reach;
		for (const auto &w : kWheels)
		{
			const int bone = natives::GetEntityBoneIndexByName(v, w.bone);
			if (bone < 0)
				continue;
			const Vector3 b = natives::GetWorldPositionOfEntityBone(v, bone);
			const float d = (b.x - x) * (b.x - x) + (b.y - y) * (b.y - y) + (b.z - z) * (b.z - z);
			if (d < bestD)
			{
				bestD = d;
				best = w.index;
			}
		}
		return best;
	}

	/// The bullets GTA fires for Minecraft (an arrow's hit, a pane's shattering) only fly once their weapon's asset is
	/// loaded, which the player's own weapons don't guarantee: keep them loaded.
	void weapon_assets_tick()
	{
		static int next = 0;
		const int now = natives::GetGameTimer();
		if (now < next)
			return;
		next = now + 2000;
		for (const Hash w : {kPistol, kSniper})
			if (!natives::HasWeaponAssetLoaded(w))
				natives::RequestWeaponAsset(w);
	}

	/// Shatter GTA glass at `at`, hit along u: a quiet, harmless bullet there, so it breaks just where it was hit, as
	/// a bullet breaks it (a pane in the world, a car's window).
	void break_glass(Ped ped, const Vector3 &at, float ux, float uy, float uz, Entity entity)
	{
		// a car's window: a side or rear one shatters (as a bullet does it), a windscreen cracks where it's hit (a real
		// bullet's worth: a token one didn't break car glass)
		if (entity != 0 && natives::GetEntityType(entity) == 2)
		{
			const int w = nearest_window(entity, at.x, at.y, at.z, 1.5f);
			if (w >= 0 && w != 6 && w != 7)
			{
				natives::SmashVehicleWindow(entity, w);
				return;
			}
			natives::ShootSingleBulletBetweenCoords(at.x - ux * 0.4f, at.y - uy * 0.4f, at.z - uz * 0.4f, at.x + ux * 0.25f, at.y + uy * 0.25f,
				at.z + uz * 0.25f, 25, kPistol, ped, FALSE);
			return;
		}
		// a pane in the world: a bullet's worth of breakage where it was hit
		natives::ShootSingleBulletBetweenCoords(at.x - ux * 0.4f, at.y - uy * 0.4f, at.z - uz * 0.4f, at.x + ux * 0.4f, at.y + uy * 0.4f,
			at.z + uz * 0.4f, 1, kPistol, ped, FALSE);
	}

	/// GTA's people near (x, y, z), not the player or a mob's double, within `radius`.
	template <typename F>
	void each_ped_near(Ped player, float x, float y, float z, float radius, F &&each)
	{
		int handles[256];
		const int n = worldGetAllPeds(handles, 256);
		for (int i = 0; i < n; ++i)
		{
			const Ped q = handles[i];
			if (q == player || g_doublePeds.count(q) || natives::IsPedDeadOrDying(q))
				continue;
			const Vector3 o = natives::GetEntityCoords(q, TRUE);
			const float dx = o.x - x, dy = o.y - y, dz = o.z - z, d = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (d < radius)
				each(q, dx, dy, d);
		}
	}

	template <typename F>
	void each_vehicle_near(float x, float y, float z, float radius, F &&each)
	{
		int handles[256];
		const int n = worldGetAllVehicles(handles, 256);
		for (int i = 0; i < n; ++i)
		{
			const Vector3 o = natives::GetEntityCoords(handles[i], TRUE);
			const float dx = o.x - x, dy = o.y - y, dz = o.z - z, d = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (d < radius)
				each(handles[i], dx, dy, d);
		}
	}

	// GTA's people and cars a potion made invisible, until when (then they show again)
	std::unordered_map<Entity, int> g_potionHidden;
	// GTA's people a speed or slowness potion hit: how fast they move, till when (GTA's own lasts a frame: set every frame)
	std::unordered_map<Ped, std::pair<float, int>> g_potionPace;

	/// A splash potion broke at `at`: harming and poison hurt the people around, healing and regeneration heal them,
	/// slowness and weakness knock them down (Minecraft's own player: Minecraft's potion).
	void potion_splash(Ped player, const Vector3 &at, const std::string &potion)
	{
		auto has = [&](const char *k) { return potion.find(k) != std::string::npos; };
		const bool harm = has("harming") || has("poison") || has("decay");
		const bool heal = has("healing") || has("regeneration");
		const bool slow = has("slowness") || has("weakness") || has("turtle");
		const bool fast = has("swiftness") || has("speed");
		const bool leap = has("leaping");
		const bool fireRes = has("fire_resistance");
		const bool invisible = has("invisibility");
		const bool levitate = has("levitation") || has("slow_falling");
		const int strength = has("strong_") ? 2 : 1;
		const int now = natives::GetGameTimer();
		each_ped_near(player, at.x, at.y, at.z, 4.5f, [&](Ped q, float dx, float dy, float d) {
			if (harm)
			{
				natives::SetPedToRagdoll(q, 1500);
				natives::ApplyDamageToPed(q, 90 * strength);
			}
			if (heal)
				natives::SetEntityHealth(q, natives::GetEntityMaxHealth(q));
			if (slow)
			{
				natives::SetPedToRagdoll(q, 2500);
				g_potionPace[q] = {0.5f, now + 20000 * strength};
			}
			if (fast)
				g_potionPace[q] = {1.6f, now + 20000 * strength};
			if (leap || levitate)
			{
				natives::SetPedToRagdoll(q, 2500);
				natives::ApplyForceToEntity(q, dx / std::max(d, 0.1f) * 2.0f, dy / std::max(d, 0.1f) * 2.0f, leap ? 9.0f * strength : 14.0f);
			}
			if (fireRes)
			{
				natives::StopEntityFire(q);
				natives::SetEntityProofs(q, FALSE, TRUE, FALSE, FALSE, FALSE);
			}
			if (invisible)
			{
				natives::SetEntityVisible(q, FALSE, FALSE);
				g_potionHidden[q] = now + 30000 * strength;
			}
		});
		each_vehicle_near(at.x, at.y, at.z, 4.5f, [&](Vehicle v, float dx, float dy, float d) {
			const float n = std::max(d, 0.1f);
			if (harm)
			{
				// eaten into: the engine and body suffer, the paint bubbles in
				natives::SetVehicleEngineHealth(v, natives::GetVehicleEngineHealth(v) - 350.0f * strength);
				natives::SetVehicleBodyHealth(v, std::max(0.0f, natives::GetVehicleBodyHealth(v) - 300.0f * strength));
				dent_vehicle(v, at, 300.0f * strength, 0.8f);
			}
			if (heal)
				natives::SetVehicleFixed(v);
			if (slow)
			{
				const Vector3 vel = natives::GetEntityVelocity(v);
				natives::ApplyForceToEntity(v, -vel.x * 0.8f, -vel.y * 0.8f, 0.0f);
			}
			if (fast)
			{
				const float h = natives::GetEntityHeading(v) * 3.14159265f / 180.0f;
				natives::ApplyForceToEntity(v, -std::sin(h) * 25.0f * strength, std::cos(h) * 25.0f * strength, 0.0f);
			}
			if (leap || levitate)
				natives::ApplyForceToEntity(v, dx / n * 2.0f, dy / n * 2.0f, leap ? 12.0f * strength : 18.0f);
			if (fireRes)
				natives::StopEntityFire(v);
			if (invisible)
			{
				natives::SetEntityVisible(v, FALSE, FALSE);
				g_potionHidden[v] = now + 30000 * strength;
			}
		});
		// (the player: Minecraft applies the potion to its own player, whose hurt or healing comes over as "pdmg"/"pheal")
	}

	/// The potion-hidden show again when it wears off.
	void potion_tick()
	{
		const int now = natives::GetGameTimer();
		for (auto it = g_potionPace.begin(); it != g_potionPace.end();)
		{
			if (now >= it->second.second || !natives::DoesEntityExist(it->first))
			{
				it = g_potionPace.erase(it);
				continue;
			}
			natives::SetPedMoveRateOverride(it->first, it->second.first);
			++it;
		}
		if (g_potionHidden.empty())
			return;
		for (auto it = g_potionHidden.begin(); it != g_potionHidden.end();)
		{
			if (now < it->second)
			{
				++it;
				continue;
			}
			if (natives::DoesEntityExist(it->first))
				natives::SetEntityVisible(it->first, TRUE, FALSE);
			it = g_potionHidden.erase(it);
		}
	}

	/// A wind charge burst at `at`: people and cars around are blown away, and the player close to it is launched
	/// (Minecraft's wind charge jump).
	void wind_burst(Ped player, const Vector3 &at)
	{
		each_ped_near(player, at.x, at.y, at.z, 4.0f, [&](Ped q, float dx, float dy, float d) {
			const float k = 1.0f - d / 4.0f, n = std::max(d, 0.1f);
			natives::SetPedToRagdoll(q, 1800);
			natives::ApplyForceToEntity(q, dx / n * 12.0f * k, dy / n * 12.0f * k, 5.0f * k + 2.0f);
		});
		each_vehicle_near(at.x, at.y, at.z, 4.5f, [&](Vehicle v, float dx, float dy, float d) {
			const float k = 1.0f - d / 4.5f, n = std::max(d, 0.1f);
			natives::ApplyForceToEntity(v, dx / n * 18.0f * k, dy / n * 18.0f * k, 6.0f * k);
			const int w = nearest_window(v, at.x, at.y, at.z, 3.0f);
			if (w >= 0)
				natives::SmashVehicleWindow(v, w);
		});
		const Vector3 me = natives::GetEntityCoords(player, TRUE);
		const float dx = me.x - at.x, dy = me.y - at.y, dz = me.z - 1.0f - at.z, d = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (d < 2.5f && !g_drive.on)
		{
			const Vector3 v = natives::GetEntityVelocity(player);
			const float n = std::max(std::sqrt(dx * dx + dy * dy), 0.1f), k = 1.0f - d / 2.5f;
			natives::SetEntityVelocity(player, v.x + dx / n * 4.0f * k, v.y + dy / n * 4.0f * k, 7.0f + 4.0f * k);
		}
		shake_from(at.x, at.y, at.z, 0.25f);
	}

	/// A level in a projectile's "sub" ("pw3:pu1:fl"): the digits after `key`, or 0.
	int sub_level(const std::string &sub, const char *key)
	{
		const size_t at = sub.find(key);
		return at == std::string::npos ? 0 : std::atoi(sub.c_str() + at + std::strlen(key));
	}

	// Minecraft's arrows stuck in GTA's people, cars and signs: kept on them (by the nearest bone, for a person)
	struct StuckArrow
	{
		int id;
		Entity on;
		int bone;          // a person's bone it's by (0: the entity itself)
		Vector3 local;     // its place from that bone (or the entity), in the entity's frame
		Vector3 dir;       // which way it points, in the entity's frame
		int until;
	};
	std::vector<StuckArrow> g_stuckArrows;

	Vector3 entity_dir_local(Entity e, float x, float y, float z)
	{
		const Vector3 c = natives::GetEntityCoords(e, TRUE);
		const Vector3 o = natives::GetOffsetFromEntityGivenWorldCoords(e, c.x, c.y, c.z);
		const Vector3 l = natives::GetOffsetFromEntityGivenWorldCoords(e, c.x + x, c.y + y, c.z + z);
		return Vector3{l.x - o.x, 0, l.y - o.y, 0, l.z - o.z, 0};
	}

	Vector3 entity_dir_world(Entity e, const Vector3 &l)
	{
		const Vector3 a = natives::GetOffsetFromEntityInWorldCoords(e, l.x, l.y, l.z), b = natives::GetOffsetFromEntityInWorldCoords(e, 0.0f, 0.0f, 0.0f);
		return Vector3{a.x - b.x, 0, a.y - b.y, 0, a.z - b.z, 0};
	}

	void arrow_stuck(int id, Entity on, bool person, float x, float y, float z, float ux, float uy, float uz)
	{
		StuckArrow a = {id, on, 0, {}, {}, natives::GetGameTimer() + 120000};
		Vector3 anchor = natives::GetEntityCoords(on, TRUE);
		if (person)
		{
			float best = 1e9f;
			for (const int b : {0x796E, 0x9995, 0x60F2, 0x60F1, 0x2E28, 0xE39F, 0xCA72, 0xF9BB, 0x9000, 0xB1C5, 0x9D4D, 0xEEEB, 0x6E5C})
			{
				const Vector3 p = natives::GetPedBoneCoords(on, b);
				const float d = (p.x - x) * (p.x - x) + (p.y - y) * (p.y - y) + (p.z - z) * (p.z - z);
				if (d < best)
				{
					best = d;
					a.bone = b;
					anchor = p;
				}
			}
		}
		a.local = entity_dir_local(on, x - anchor.x, y - anchor.y, z - anchor.z);
		a.dir = entity_dir_local(on, ux, uy, uz);
		g_stuckArrows.erase(std::remove_if(g_stuckArrows.begin(), g_stuckArrows.end(), [&](const StuckArrow &s) { return s.id == id; }),
			g_stuckArrows.end());
		g_stuckArrows.push_back(a);
		if (g_stuckArrows.size() > 64)
			g_stuckArrows.erase(g_stuckArrows.begin());
	}

	/// Every other frame: where each stuck arrow is now ({"t":"arrows","a":[[id,x,y,z,yaw,pitch],...],"gone":[ids]}).
	void arrows_tick()
	{
		if (g_stuckArrows.empty() || natives::GetFrameCount() % 2 != 0)
			return;
		const int now = natives::GetGameTimer();
		std::string out = "{\"t\":\"arrows\",\"a\":[", gone;
		bool first = true;
		for (auto it = g_stuckArrows.begin(); it != g_stuckArrows.end();)
		{
			if (now > it->until || !natives::DoesEntityExist(it->on))
			{
				gone += (gone.empty() ? "" : ",") + std::to_string(it->id);
				it = g_stuckArrows.erase(it);
				continue;
			}
			const Vector3 anchor = it->bone != 0 ? natives::GetPedBoneCoords(it->on, it->bone) : natives::GetEntityCoords(it->on, TRUE);
			const Vector3 off = entity_dir_world(it->on, it->local), d = entity_dir_world(it->on, it->dir);
			const float x = anchor.x + off.x, y = anchor.y + off.y, z = anchor.z + off.z;
			// Minecraft's arrow yaw/pitch for flying along d (Minecraft: x, z = -y)
			const float mx = d.x, mz = -d.y, my = d.z;
			const float yaw = std::atan2(mx, mz) * 57.2958f, pitch = std::atan2(my, std::sqrt(mx * mx + mz * mz)) * 57.2958f;
			char e[96];
			snprintf(e, sizeof(e), "%s[%d,%.3f,%.3f,%.3f,%.1f,%.1f]", first ? "" : ",", it->id, x, z + g_yOffset, -y, yaw, pitch);
			out += e;
			first = false;
			++it;
		}
		out += "],\"gone\":[" + gone + "]}";
		g_ws.send(out);
	}

	/// A Minecraft projectile went from where it was to (bx..) this tick (GTA coordinates): if that crosses anything of
	/// GTA's, it acts there (a firework blows up, an arrow or trident lands a bullet in a person or car or sticks in a
	/// wall, a potion splashes, a wind charge bursts, ...), and Minecraft is told where (so it ends there too). GTA glass
	/// on the way shatters and the projectile flies on.
	/// A splash in GTA's water where something of Minecraft's went in (an arrow, a trident, a pearl, a bobber): GTA's own
	/// splash, a harmless shot into the water there (nobody's: no wanted level for it).
	void water_splash(float x, float y, float surface)
	{
		natives::ShootSingleBulletBetweenCoords(x, y, surface + 1.2f, x, y, surface - 0.6f, 0, kPistol, 0, FALSE);
	}

	bool projectile_segment(Ped ped, int id, Projectile &pr, float bx, float by, float bz)
	{
		// GTA's water crossed on the way (its surface between where it was and where it is): a splash where it went in.
		// A fishing bobber floats there (Minecraft's water physics have it): traced on, it hit the bed below and was
		// ended there, so fishing in GTA's sea, lakes and rivers never worked
		if (!pr.wet)
		{
			float surface = 0.0f;
			if (water_surface(bx, by, std::min(pr.z, bz) - 2.0f, std::max(pr.z, bz) + 2.0f, surface) && bz < surface && pr.z >= surface - 0.05f)
			{
				const float t = pr.z - bz > 1e-3f ? std::clamp((pr.z - surface) / (pr.z - bz), 0.0f, 1.0f) : 0.0f;
				const float wx = pr.x + (bx - pr.x) * t, wy = pr.y + (by - pr.y) * t;
				pr.wet = true;
				water_splash(wx, wy, surface);
				logf("%s into GTA's water at %.1f %.1f (surface %.2f): splash", pr.kind.c_str(), wx, wy, surface);
				if (pr.kind == "bobber")
				{
					// (what's above the water on the way still gets hooked: traced to the surface only)
					bx = wx;
					by = wy;
					bz = surface + 0.05f;
				}
			}
		}
		else if (pr.kind == "bobber")
			return false; // (bobbing on GTA's water: Minecraft's own fishing)
		BOOL hit = FALSE;
		Vector3 end = {}, normal = {};
		Entity entity = 0;
		Hash material = 0;
		float sx0 = pr.x, sy0 = pr.y, sz0 = pr.z;
		// (shot from a car: the car itself isn't in the way)
		Entity ignore = natives::IsPedInAnyVehicle(ped, FALSE) ? Entity(natives::GetVehiclePedIsIn(ped, FALSE)) : Entity(ped);
		const float dx = bx - pr.x, dy = by - pr.y, dz = bz - pr.z;
		const float len = std::max(0.001f, std::sqrt(dx * dx + dy * dy + dz * dz));
		const float ux = dx / len, uy = dy / len, uz = dz / len;
		for (int tries = 0;; ++tries)
		{
			const int probe = natives::StartShapeTestLosProbe(sx0, sy0, sz0, bx, by, bz, 1 | 2 | 4 | 8 | 16 | 64, ignore, kSeeGlass);
			if (natives::GetShapeTestResultIncludingMaterial(probe, &hit, &end, &normal, &material, &entity) != 2)
				hit = FALSE;
			// a person on the way, short of what the line met: GTA's far-off people have no collision for a line (only
			// those near the player do), so an arrow flew through anyone more than a street away
			{
				const float limit = hit ? (end.x - sx0) * ux + (end.y - sy0) * uy + (end.z - sz0) * uz : (bx - sx0) * ux + (by - sy0) * uy + (bz - sz0) * uz;
				float best = 0.45f;
				Ped on = 0;
				Vector3 onAt = {};
				int handles[256];
				const int n = worldGetAllPeds(handles, 256);
				// (someone in a car: GTA's line meets the car, never them. Past its side, window or bonnet, up to 3 m on, it's them)
				const Entity hitCar = hit && entity != 0 && natives::GetEntityType(entity) == 2 ? entity : 0;
				for (int i = 0; i < n; ++i)
				{
					const Ped q = handles[i];
					if (q == ped || Entity(q) == ignore || g_doublePeds.count(q))
						continue;
					const Entity car = natives::IsPedInAnyVehicle(q, FALSE) ? Entity(natives::GetVehiclePedIsIn(q, FALSE)) : 0;
					if (car != 0 && car == ignore)
						continue;
					const Vector3 o = natives::GetEntityCoords(q, TRUE);
					const float px = o.x - sx0, py = o.y - sy0, pz = o.z - sz0;
					const float t = px * ux + py * uy + pz * uz;
					if (t < 0.0f || t > limit + (car != 0 && car == hitCar ? 3.0f : 0.0f))
						continue;
					const float lx = px - ux * t, ly = py - uy * t, lz = (pz - uz * t) * 0.5f; // (a person is tall: up and down counts less)
					const float off = std::sqrt(lx * lx + ly * ly + lz * lz);
					if (off < best)
					{
						best = off;
						on = q;
						onAt = Vector3{sx0 + ux * t, 0, sy0 + uy * t, 0, sz0 + uz * t, 0};
					}
				}
				if (on != 0)
				{
					// through the window on the way in: it breaks
					if (hitCar != 0 && natives::IsPedInAnyVehicle(on, FALSE) && Entity(natives::GetVehiclePedIsIn(on, FALSE)) == hitCar &&
						(pr.kind == "arrow" || pr.kind == "trident") && nearest_window(hitCar, end.x, end.y, end.z, 0.35f) >= 0)
						break_glass(ped, end, ux, uy, uz, hitCar);
					hit = TRUE;
					entity = on;
					end = onAt;
					material = 0;
				}
			}
			if (!hit)
				return false; // (a bobber on the water among it: it floats)
			// one of Minecraft's own blocks (its prop): Minecraft stops the projectile there itself. (Reporting it too
			// put the arrow a little off the block, and the two sides fought over it: it jittered about.)
			if (entity != 0 && g_props.handles.count(entity))
				return false;
			// something carried or attached (the player's own bag or phone, a ped's held thing): not what it hit, trace on
			// (an arrow caught on one right after it was shot seemed to vanish)
			const bool carried = entity != 0 && natives::GetEntityType(entity) == 3 && natives::IsEntityAttached(entity);
			const bool doubled = entity != 0 && (g_doublePeds.count(entity) || carried);
			const int hitType = entity != 0 ? natives::GetEntityType(entity) : 0;
			bool glass = !doubled && pr.kind != "pearl" && hitType != 1 && breakable_glass(material);
			// an arrow at a car's window goes through it, whatever material GTA reports there
			if (!glass && !doubled && hitType == 2 && (pr.kind == "arrow" || pr.kind == "trident") &&
				nearest_window(entity, end.x, end.y, end.z, 0.35f) >= 0)
				glass = true;
			if (!doubled && !glass)
				break;
			// a Minecraft mob's double (Minecraft resolves hits on its own mobs), or glass (shattered, a car's window
			// too, so an arrow goes on into the car): trace on past it
			if (glass)
				break_glass(ped, end, ux, uy, uz, entity);
			if (tries >= 3)
				return false;
			const float ex = bx - end.x, ey = by - end.y, ez = bz - end.z, el = std::sqrt(ex * ex + ey * ey + ez * ez);
			if (el < 0.4f)
				return false;
			sx0 = end.x + ex / el * 0.3f;
			sy0 = end.y + ey / el * 0.3f;
			sz0 = end.z + ez / el * 0.3f;
			if (doubled)
				ignore = entity;
		}
		const int type = entity != 0 ? natives::GetEntityType(entity) : 0; // 1 ped, 2 vehicle, 3 object
		bool stick = false;
		if (pr.kind == "firework")
		{
			natives::AddExplosion(end.x, end.y, end.z, 4 /* rocket */, 1.0f, TRUE, FALSE, 0.0f, FALSE);
			shake_from(end.x, end.y, end.z, 0.6f);
		}
		else if (pr.kind == "arrow" || pr.kind == "trident")
		{
			const bool trident = pr.kind == "trident";
			// the bow's enchantments: power hurts more, punch knocks people down, flame sets them alight
			const int power = sub_level(pr.sub, "pw"), punch = sub_level(pr.sub, "pu");
			const bool flame = pr.sub.find("fl") != std::string::npos;
			// Impaling: a trident's extra bite on what's in the water or out in the rain (as Minecraft's Bedrock has it:
			// GTA's people and cars in its sea, or a rainy street)
			const int impaling = trident ? sub_level(pr.sub, "im") : 0;
			const bool wetTarget = impaling > 0 && (type == 1 || type == 2) &&
				(natives::IsEntityInWater(entity) || natives::GetRainLevel() > 0.15f);
			const float bite = wetTarget ? 1.0f + 0.4f * float(impaling) : 1.0f;
			if (type == 1 || type == 2)
				mc_hit(entity, true);
			int damage = int((trident ? 450 : 250) * (1.0f + 0.25f * float(power)) * bite);
			// the bow at a person: two hits kill, one to the head (a sniper's bullet's worth killed with any hit)
			const bool bowAtPerson = type == 1 && !trident;
			if (bowAtPerson)
			{
				const Vector3 head = natives::GetPedBoneCoords(entity, 31086);
				const float hx = head.x - end.x, hy = head.y - end.y, hz = head.z - end.z, t = hx * ux + hy * uy + hz * uz;
				const float ox = hx - ux * t, oy = hy - uy * t, oz = hz - uz * t;
				const bool headshot = ox * ox + oy * oy + oz * oz < 0.22f * 0.22f;
				damage = headshot ? 10000 : std::max(1, natives::GetEntityMaxHealth(entity) - 100) / 2 + 1;
			}
			const bool knocked = type == 1 && (punch > 0 || trident) && !natives::IsPedDeadOrDying(entity);
			if (knocked)
			{
				// punch (or a trident's weight): knocked flying first, hurt after (killed first, they only dropped)
				const float f = (trident ? 10.0f : 0.0f) + 6.0f * float(punch);
				knock_ped(entity, ux * f, uy * f, trident ? 3.0f : 2.5f, trident ? 3000 : 2500, damage);
			}
			else if (type == 1 || type == 2)
			{
				natives::ShootSingleBulletBetweenCoords(end.x - ux * 1.0f, end.y - uy * 1.0f, end.z - uz * 1.0f,
					end.x + ux * 0.6f, end.y + uy * 0.6f, end.z + uz * 0.6f, bowAtPerson ? 1 : damage, kSniper,
					ped, FALSE); // (silent: an arrow, not a sniper's shot; at a person only its blood and flinch)
				if (bowAtPerson)
					natives::ApplyDamageToPed(entity, damage);
			}
			else
				stick = true;
			if ((type == 1 || type == 2) && flame)
				natives::StartEntityFire(entity);
			if (type == 2)
			{
				// in a wheel: that tyre goes flat (the bullet alone only did when it happened to hit the rubber)
				const int tyre = nearest_tyre(entity, end.x, end.y, end.z, 0.6f);
				if (tyre >= 0 && natives::GetVehicleTyresCanBurst(entity) && !natives::IsVehicleTyreBurst(entity, tyre, FALSE))
					natives::SetVehicleTyreBurst(entity, tyre, FALSE, 1000.0f);
			}
			if (trident && pr.sub.find("chan") != std::string::npos)
			{
				// channeling: lightning strikes where it lands
				natives::ForceLightningFlash();
				natives::AddExplosion(end.x, end.y, end.z, 36 /* railgun */, 1.0f, TRUE, FALSE, 0.0f, FALSE);
				shake_from(end.x, end.y, end.z, 0.7f);
			}
		}
		else if (pr.kind == "snowball" || pr.kind == "egg")
		{
			if (type == 1)
			{
				natives::SetPedToRagdoll(entity, 900);
				natives::ApplyDamageToPed(entity, 5);
				natives::ApplyForceToEntity(entity, ux * 4.0f, uy * 4.0f, 1.5f);
			}
		}
		else if (pr.kind == "potion")
			potion_splash(ped, end, pr.sub);
		else if (pr.kind == "wind")
			wind_burst(ped, end);
		else if (pr.kind == "bobber" && (type == 2 || type == 3))
		{
			// hooked a car or a loose thing: dragged a little towards the player
			const Vector3 me = natives::GetEntityCoords(ped, TRUE);
			const float hx = me.x - end.x, hy = me.y - end.y, hl = std::max(0.1f, std::sqrt(hx * hx + hy * hy));
			const Entity hooked = type == 3 ? Entity(loosen(entity, 300.0f)) : entity;
			if (hooked != 0)
			{
				if (type == 3)
					natives::SetEntityDynamic(hooked, TRUE);
				natives::ApplyForceToEntity(hooked, hx / hl * (type == 2 ? 14.0f : 6.0f), hy / hl * (type == 2 ? 14.0f : 6.0f), type == 2 ? 1.0f : 2.0f);
			}
		}
		else if (pr.kind == "bobber" && type == 1)
		{
			// hooked: reeled in towards the player
			const Vector3 me = natives::GetEntityCoords(ped, TRUE);
			const float hx = me.x - end.x, hy = me.y - end.y, hl = std::max(0.1f, std::sqrt(hx * hx + hy * hy));
			natives::SetPedToRagdoll(entity, 2000);
			natives::ApplyForceToEntity(entity, hx / hl * 11.0f, hy / hl * 11.0f, 4.0f);
		}
		float sx = end.x - ux * 0.15f, sy = end.y - uy * 0.15f, sz = end.z - uz * 0.15f;
		if (pr.kind == "pearl")
		{
			// Steve lands there: clear of a wall (he's 0.6 m wide), under a ceiling rather than in it
			const float nl = std::sqrt(normal.x * normal.x + normal.y * normal.y);
			if (nl > 0.5f)
			{
				sx = end.x + normal.x / nl * 0.35f;
				sy = end.y + normal.y / nl * 0.35f;
			}
			if (normal.z < -0.5f)
				sz = end.z - 1.85f;
			// and never inside anything: room for him (0.6 m wide, 1.8 m tall) checked there, and nudged out of what's in
			// the way (off the wall it hit, down from a ceiling, up from a floor or a slope), up to a metre
			auto blocked = [&](float x, float y, float z, Vector3 &at, Vector3 &n) {
				return solid_probe(ped, x, y, z + 0.05f, x, y, z + 1.75f, at, n) || solid_probe(ped, x - 0.3f, y, z + 0.9f, x + 0.3f, y, z + 0.9f, at, n) ||
					solid_probe(ped, x + 0.3f, y, z + 0.9f, x - 0.3f, y, z + 0.9f, at, n) || solid_probe(ped, x, y - 0.3f, z + 0.9f, x, y + 0.3f, z + 0.9f, at, n) ||
					solid_probe(ped, x, y + 0.3f, z + 0.9f, x, y - 0.3f, z + 0.9f, at, n);
			};
			Vector3 at = {}, n = {};
			for (int step = 0; step < 4 && blocked(sx, sy, sz, at, n); ++step)
			{
				if (n.z > 0.5f)
					sz = std::max(sz, at.z + 0.02f); // a floor or a slope under his feet: on it
				else if (n.z < -0.5f)
					sz = at.z - 1.8f; // a ceiling: under it
				else
				{
					const float l = std::max(0.01f, std::sqrt(n.x * n.x + n.y * n.y));
					sx += n.x / l * 0.25f; // a wall: off it
					sy += n.y / l * 0.25f;
				}
			}
		}
		if ((pr.kind == "arrow" || pr.kind == "trident") && type >= 1 && type <= 3 && entity != 0)
		{
			// in a person, a car or a sign: it stays in them, and goes where they go (as Minecraft's arrows in its mobs)
			stick = true;
			bool flung = false;
			if (type == 3)
			{
				const int punch = pr.kind == "arrow" ? sub_level(pr.sub, "pu") : 0;
				if (punch > 0 || pr.kind == "trident")
				{
					// punch (or a trident): a sign, a bin, a lamp post knocked loose and sent flying (the arrow drops)
					const float light = std::clamp(std::sqrt(60.0f / std::max(1.0f, entity_mass(entity))), 0.35f, 1.5f);
					const float speed = std::min(40.0f, (4.0f + 5.0f * float(punch) + (pr.kind == "trident" ? 6.0f : 0.0f)) * light);
					launch_object(entity, ux * speed, uy * speed, 1.5f + uz * speed, 3000.0f);
					flung = true;
					stick = false;
				}
				// a sign, a bin: knocked a little (a light thing loose; the map's own as the plugin's copy, it stuck in that)
				else if (const Object o = loosen(entity, 300.0f))
				{
					entity = o;
					launch_object(o, ux * 1.5f, uy * 1.5f, 0.2f, 300.0f);
				}
			}
			if (!flung)
				arrow_stuck(id, entity, type == 1, sx, sy, sz, ux, uy, uz);
		}
		sendf("{\"t\":\"projhit\",\"id\":%d,\"pos\":[%.3f,%.3f,%.3f],\"stick\":%s}", id, sx, sz + g_yOffset, -sy, stick ? "true" : "false");
		return true;
	}

	/// A projectile Minecraft ended itself (a firework's burst in the air, a potion or wind charge on a Minecraft
	/// block or mob): it acts on GTA's side too, where it was last.
	void projectile_gone(Ped ped, const Projectile &pr)
	{
		if (pr.done)
			return;
		Vector3 at = {};
		at.x = pr.x;
		at.y = pr.y;
		at.z = pr.z;
		if ((pr.kind == "arrow" || pr.kind == "trident") && pr.ux * pr.ux + pr.uy * pr.uy + pr.uz * pr.uz > 0.5f)
		{
			// stopped by Minecraft (its stand-in for GTA's walls is a little short of the glass in them): glass just ahead
			// breaks
			BOOL hit = FALSE;
			Vector3 end = {}, normal = {};
			Entity entity = 0;
			Hash material = 0;
			const int probe = natives::StartShapeTestLosProbe(at.x - pr.ux * 0.3f, at.y - pr.uy * 0.3f, at.z - pr.uz * 0.3f,
				at.x + pr.ux * 1.6f, at.y + pr.uy * 1.6f, at.z + pr.uz * 1.6f, kGlassFlags, ped, kSeeGlass);
			if (natives::GetShapeTestResultIncludingMaterial(probe, &hit, &end, &normal, &material, &entity) == 2 && hit &&
				(breakable_glass(material) || (entity != 0 && natives::GetEntityType(entity) == 2 && nearest_window(entity, end.x, end.y, end.z, 0.5f) >= 0)))
				break_glass(ped, end, pr.ux, pr.uy, pr.uz, entity);
		}
		if (pr.kind == "firework")
		{
			natives::AddExplosion(at.x, at.y, at.z, 4 /* rocket */, 1.0f, TRUE, FALSE, 0.0f, FALSE);
			shake_from(at.x, at.y, at.z, 0.6f);
		}
		else if (pr.kind == "potion")
			potion_splash(ped, at, pr.sub);
		else if (pr.kind == "wind")
			wind_burst(ped, at);
	}

	/// {"t":"proj","p":[[id,"kind[:sub]",x,y,z],...]} (Minecraft coordinates), every tick while any fly, and once
	/// empty when the last one is gone.
	void projectiles_message(Ped ped, const std::string &message)
	{
		for (auto &[id, pr] : g_projectiles)
			pr.seen = false;
		const char *at = json_value(message, "p");
		const Vector3 me = natives::GetEntityCoords(ped, TRUE);
		while (at && (at = std::strchr(at + 1, '[')) != nullptr)
		{
			int id = 0;
			char kind[64] = {};
			double x, y, z;
			if (sscanf_s(at, "[%d,\"%63[^\"]\",%lf,%lf,%lf]", &id, kind, unsigned(sizeof(kind)), &x, &y, &z) != 5)
				continue;
			const float gx = float(x), gy = float(-z), gz = float(y) - g_yOffset;
			auto it = g_projectiles.find(id);
			if (it == g_projectiles.end())
			{
				// first sighting: trace from Steve's chest, so point-blank shots count
				std::string k = kind, sub;
				if (const size_t colon = k.find(':'); colon != std::string::npos)
				{
					sub = k.substr(colon + 1);
					k.resize(colon);
				}
				// (first seen well away from Steve, after a reconnect or shot by something else: traced from there on, not
				// from his chest across everything between)
				const float fx = gx - me.x, fy = gy - me.y, fz = gz - me.z;
				const bool closeBy = fx * fx + fy * fy + fz * fz < 10.0f * 10.0f; // (a shot flies 3 m a tick: its first report can be that far)
				it = g_projectiles.emplace(id, Projectile{closeBy ? me.x : gx, closeBy ? me.y : gy, closeBy ? me.z + 0.4f : gz, k, sub, false, false}).first;
			}
			Projectile &pr = it->second;
			pr.seen = true;
			if (g_noInput)
				pr.done = true; // a cutscene or a mission's scene: what was in flight leaves GTA's world alone
			if (!pr.done && projectile_segment(ped, id, pr, gx, gy, gz))
				pr.done = true;
			{
				const float mx = gx - pr.x, my = gy - pr.y, mz = gz - pr.z, ml = std::sqrt(mx * mx + my * my + mz * mz);
				if (ml > 0.02f)
				{
					pr.ux = mx / ml;
					pr.uy = my / ml;
					pr.uz = mz / ml;
				}
				// (one that stops is gone from Minecraft's list at once: projectile_gone checks for glass just ahead)
			}
			pr.x = gx;
			pr.y = gy;
			pr.z = gz;
		}
		for (auto it = g_projectiles.begin(); it != g_projectiles.end();)
		{
			if (it->second.seen)
			{
				++it;
				continue;
			}
			projectile_gone(ped, it->second);
			it = g_projectiles.erase(it);
		}
	}

	/// A crew member who fights the player (it keeps at it rather than fleeing), armed with a carbine rifle.
	void army_crew(Ped crew, Ped target)
	{
		natives::SetEntityAsMissionEntity(crew);
		natives::GiveWeaponToPed(crew, 0x83BF0278, 9999, FALSE, TRUE);
		natives::SetPedCombatAttributes(crew, 46, TRUE); // always fight
		natives::SetPedCombatAttributes(crew, 5, TRUE);  // fight armed peds
		natives::SetPedFleeAttributes(crew, 0, FALSE);
		natives::TaskCombatPed(crew, target);
		natives::SetPedKeepTask(crew, TRUE);
		g_army.push_back(crew);
	}

	/// GTA's ped type for a model by its name: police (6), army (29) or a civilian (26), so a summoned cop is a cop.
	int ped_type(const std::string &model)
	{
		std::string m = model;
		for (char &ch : m)
			ch = char(std::tolower(static_cast<unsigned char>(ch)));
		if (m.rfind("a_c_", 0) == 0)
			return 28; // an animal
		for (const char *cop : {"s_m_y_cop", "s_f_y_cop", "s_m_y_sheriff", "s_f_y_sheriff", "s_m_y_hwaycop", "s_m_m_snowcop", "s_m_y_ranger",
				 "s_f_y_ranger", "csb_cop", "s_m_m_ciasec", "s_m_m_fibsec", "s_m_y_swat"})
			if (m.rfind(cop, 0) == 0)
				return 6;
		for (const char *army : {"s_m_y_marine", "s_m_m_marine", "s_m_y_blackops", "s_m_y_armymech", "s_m_m_pilot"})
			if (m.rfind(army, 0) == 0)
				return 29;
		return 26;
	}

	Hash army_model(const char *name)
	{
		const Hash h = natives::GetHashKey(name);
		natives::RequestModel(h);
		for (int i = 0; i < 300 && !natives::HasModelLoaded(h); ++i)
			WAIT(0);
		return natives::HasModelLoaded(h) ? h : 0;
	}

	/// Tanks on the roads around the player and attack helicopters above, crewed by marines who fight the player.
	void army_spawn(Ped ped, int tanks, int helis, float dist)
	{
		const Hash rhino = army_model("rhino"), buzzard = army_model("buzzard"), marine = army_model("s_m_y_marine_01");
		if (marine == 0)
			return;
		const Vector3 me = natives::GetEntityCoords(ped, TRUE);
		for (int i = 0; i < tanks && rhino != 0; ++i)
		{
			const float a = (0.3f + 6.2832f * i / std::max(1, tanks));
			Vector3 at = {};
			float heading = 0.0f;
			if (!natives::GetClosestVehicleNodeWithHeading(me.x + std::cos(a) * dist, me.y + std::sin(a) * dist, me.z, &at, &heading))
				continue;
			const Vehicle tank = natives::CreateVehicle(rhino, at.x, at.y, at.z + 0.5f, heading);
			natives::SetEntityAsMissionEntity(tank);
			natives::SetVehicleEngineOn(tank, TRUE);
			g_army.push_back(tank);
			army_crew(natives::CreatePedInsideVehicle(tank, marine, -1), ped);
		}
		for (int i = 0; i < helis && buzzard != 0; ++i)
		{
			const float a = (1.2f + 6.2832f * i / std::max(1, helis));
			const Vehicle heli = natives::CreateVehicle(buzzard, me.x + std::cos(a) * dist, me.y + std::sin(a) * dist, me.z + 45.0f, 0.0f);
			natives::SetEntityAsMissionEntity(heli);
			natives::SetVehicleEngineOn(heli, TRUE);
			natives::SetHeliBladesFullSpeed(heli);
			g_army.push_back(heli);
			for (int seat = -1; seat <= 2; ++seat)
				if (seat != 0)
					army_crew(natives::CreatePedInsideVehicle(heli, marine, seat), ped);
		}
		for (const Hash h : {rhino, buzzard, marine})
			if (h != 0)
				natives::SetModelAsNoLongerNeeded(h);
	}

	void army_clear()
	{
		for (Entity e : g_army)
			natives::DeleteEntity(&e);
		g_army.clear();
	}

	bool mobs_active()
	{
		return !g_mobs.empty() && natives::GetGameTimer() - g_mobsSeenAt < 3000;
	}

	void mob_group_init()
	{
		if (g_mobGroup != 0)
			return;
		natives::AddRelationshipGroup("MCMOBS", &g_mobGroup);
		// one-sided: the police and the army hate Minecraft's mobs (the doubles themselves never pick fights)
		natives::SetRelationshipBetweenGroups(5, kCopGroup, g_mobGroup);
		natives::SetRelationshipBetweenGroups(5, kArmyGroup, g_mobGroup);
	}

	void double_visibility(Ped d)
	{
		natives::SetEntityVisible(d, g_doubleVis != 0 ? TRUE : FALSE, FALSE);
		natives::SetEntityAlpha(d, g_doubleVis == 2 ? 0 : 255);
	}

	/// A double for a mob at GTA feet position (x, y, z), or 0 while its model loads.
	Ped double_create(float x, float y, float z)
	{
		static const Hash model = natives::GetHashKey("a_m_y_skater_01");
		if (!natives::HasModelLoaded(model))
		{
			natives::RequestModel(model);
			return 0;
		}
		mob_group_init();
		const Ped d = natives::CreatePed(26, model, x, y, z + 1.0f, 0.0f);
		if (d == 0)
			return 0;
		natives::SetEntityAsMissionEntity(d);
		natives::SetPedRelationshipGroupHash(d, g_mobGroup);
		natives::SetBlockingOfNonTemporaryEvents(d, TRUE);
		natives::SetPedCanRagdoll(d, FALSE);
		natives::SetPedSuffersCriticalHits(d, FALSE);
		natives::SetPedDiesWhenInjured(d, FALSE);
		natives::SetPedArmour(d, 0);
		natives::SetPedMaxHealth(d, kDoubleHealth);
		natives::SetEntityHealth(d, kDoubleHealth);
		// bullets and explosions register; fire, cars and fists don't
		natives::SetEntityProofs(d, FALSE, TRUE, FALSE, TRUE, TRUE);
		natives::FreezeEntityPosition(d, TRUE);
		double_visibility(d);
		g_doublePeds.insert(d);
		return d;
	}

	void double_delete(MobDouble &m)
	{
		if (m.ped != 0)
		{
			g_doublePeds.erase(m.ped);
			if (natives::DoesEntityExist(m.ped))
				natives::DeletePed(&m.ped);
			m.ped = 0;
		}
	}

	bool near_recent_boom(const MobDouble &m)
	{
		const int now = natives::GetGameTimer();
		for (const RecentBoom &b : g_booms)
		{
			const float dx = b.x - m.x, dy = b.y - m.y, dz = b.z - m.z;
			if (b.until > now && dx * dx + dy * dy + dz * dz < 10.0f * 10.0f)
				return true;
		}
		return false;
	}

	/// {"t":"mobs","m":[[id,"zombie",x,y,z],...]} (Minecraft coordinates, feet), every server tick while any fight.
	void mobs_message(const std::string &message)
	{
		const char *p = json_value(message, "m");
		if (p == nullptr || *p != '[')
			return;
		for (auto &[id, m] : g_mobs)
			m.seen = false;
		++p;
		while (*p != '\0')
		{
			while (*p == ' ' || *p == ',')
				++p;
			if (*p != '[')
				break; // the end of the list
			int id = 0;
			char kind[32] = {};
			double x, y, z;
			if (sscanf_s(p, "[%d,\"%31[^\"]\",%lf,%lf,%lf]", &id, kind, unsigned(sizeof(kind)), &x, &y, &z) == 5)
			{
				MobDouble &m = g_mobs[id];
				m.x = float(x);
				m.y = float(-z);
				m.z = float(y) - g_yOffset;
				m.seen = true;
			}
			const char *end = std::strchr(p, ']');
			if (end == nullptr)
				break;
			p = end + 1;
		}
		for (auto it = g_mobs.begin(); it != g_mobs.end();)
		{
			if (it->second.seen)
			{
				++it;
				continue;
			}
			double_delete(it->second);
			it = g_mobs.erase(it);
		}
		if (!g_mobs.empty())
			g_mobsSeenAt = natives::GetGameTimer();
	}

	/// A Minecraft mob hit one of GTA's people: {"t":"mobhit","h":ped,"d":damage,"from":[x,y,z],"k":"zombie"}.
	void mobhit_message(Ped player, const std::string &message)
	{
		const Ped victim = Ped(json_num(message, "h", 0));
		if (victim == 0 || victim == player || !g_pedsSent.count(victim) || g_doublePeds.count(victim) ||
			!natives::DoesEntityExist(victim) || natives::GetEntityType(victim) != 1 || natives::IsPedDeadOrDying(victim))
			return;
		const float damage = float(json_num(message, "d", 2.0)) * g_mobHitScale;
		natives::ApplyDamageToPed(victim, std::max(1, int(damage)));
		++g_statHits;
		const char *from = json_value(message, "from");
		double fx = 0, fy = 0, fz = 0;
		if (from == nullptr || sscanf_s(from, "[%lf ,%lf ,%lf ]", &fx, &fy, &fz) != 3)
			return;
		const std::string kind = json_str(message, "k");
		if (kind == "ghast" || kind == "blaze" || kind == "magma_cube")
			natives::StartEntityFire(victim); // fireballs and molten slime
		if (kind == "skeleton" || kind == "stray" || kind == "bogged" || kind == "pillager" || kind == "blaze" || kind == "ghast")
			return; // arrows and fireballs: the damage is enough, no shove
		const Vector3 v = natives::GetEntityCoords(victim, TRUE);
		float dx = v.x - float(fx), dy = v.y - float(-fz);
		const float len = std::max(0.01f, std::sqrt(dx * dx + dy * dy));
		dx /= len;
		dy /= len;
		const bool heavy = kind == "iron_golem" || kind == "ravager" || kind == "warden";
		natives::SetPedToRagdoll(victim, heavy ? 3000 : 1200);
		natives::ApplyForceToEntity(victim, dx * (heavy ? 14.0f : 5.0f), dy * (heavy ? 14.0f : 5.0f), heavy ? 10.0f : 2.5f);
	}

	Hash squad_model(const char *name)
	{
		const Hash h = natives::GetHashKey(name);
		if (!natives::HasModelLoaded(h))
			natives::RequestModel(h);
		return h;
	}

	/// One police car at a road node `g_copsDist` ahead of the player, siren on, with two armed cops beside it.
	bool squad_spawn_one(Ped player)
	{
		const Hash car = squad_model("police3"), cop = squad_model("s_m_y_cop_01");
		if (!natives::HasModelLoaded(car) || !natives::HasModelLoaded(cop))
			return false;
		mob_group_init();
		const Vector3 me = natives::GetEntityCoords(player, TRUE);
		Vector3 at = {};
		float heading = 0.0f;
		if (g_copsLine)
		{
			// a barricade: cars side by side across the view, `g_copsDist` ahead, cops on the near side
			const float camH = natives::GetGameplayCamRot(2).z, h = camH * 3.14159265f / 180.0f;
			const int k = int(g_squadCars.size());
			const float lateral = (k % 2 == 0 ? 1.0f : -1.0f) * 5.5f * float((k + 1) / 2);
			const float fx = -std::sin(h), fy = std::cos(h), rx = std::cos(h), ry = std::sin(h);
			at.x = me.x + fx * g_copsDist + rx * lateral;
			at.y = me.y + fy * g_copsDist + ry * lateral;
			float g = 0.0f;
			if (!natives::GetGroundZFor3dCoord(at.x, at.y, me.z + 3.0f, &g, FALSE, FALSE) || g == 0.0f)
				return true;
			at.z = g;
			heading = camH + 90.0f + (k % 2 == 0 ? 8.0f : -8.0f);
		}
		else
		{
			const float h = (natives::GetGameplayCamRot(2).z + (float(g_squadCars.size() % 3) - 1.0f) * 35.0f) * 3.14159265f / 180.0f;
			if (!natives::GetClosestVehicleNodeWithHeading(me.x - std::sin(h) * g_copsDist, me.y + std::cos(h) * g_copsDist, me.z, &at, &heading))
				return true; // no road there: give up on this one
		}
		const Vehicle v = natives::CreateVehicle(car, at.x, at.y, at.z + 0.5f, heading);
		if (v != 0)
		{
			natives::SetEntityAsMissionEntity(v);
			natives::SetVehicleSiren(v, TRUE);
			g_squadCars.push_back(v);
		}
		const float hr = heading * 3.14159265f / 180.0f;
		const float rx = std::cos(hr), ry = std::sin(hr), fx = -std::sin(hr), fy = std::cos(hr);
		const Hash guns[] = {0x83BF0278 /* carbine */, 0x1D073A89 /* pump shotgun */, 0x1B06D571 /* pistol */};
		for (int i = 0; i < 2; ++i)
		{
			float px, py;
			float ch = heading;
			if (g_copsLine)
			{
				// the car's right side (rx, ry) faces the horde here: stand 2 m on the other side, spread along the car
				px = at.x - rx * 2.2f + fx * (i == 0 ? -1.4f : 1.4f);
				py = at.y - ry * 2.2f + fy * (i == 0 ? -1.4f : 1.4f);
				ch = heading - 90.0f;
			}
			else
			{
				const float side = i == 0 ? -2.2f : 2.2f;
				px = at.x + rx * side + fx * 0.8f;
				py = at.y + ry * side + fy * 0.8f;
			}
			const Ped c = natives::CreatePed(6, cop, px, py, at.z + 1.0f, ch);
			if (c == 0)
				continue;
			natives::SetEntityAsMissionEntity(c);
			natives::SetPedRelationshipGroupHash(c, kCopGroup);
			natives::GiveWeaponToPed(c, guns[(g_squad.size() + i) % 3], 9999, FALSE, TRUE);
			natives::SetPedAccuracy(c, 35);
			natives::SetPedCombatAbility(c, 2);
			natives::SetPedCombatMovement(c, 2);
			natives::SetPedCombatRange(c, 1);
			natives::SetPedCombatAttributes(c, 5, TRUE);  // always fight
			natives::SetPedCombatAttributes(c, 46, TRUE); // fight armed peds even when not armed
			natives::SetPedCombatAttributes(c, 58, TRUE); // don't flee from combat
			natives::SetPedFleeAttributes(c, 0, FALSE);
			natives::SetPedSeeingRange(c, 120.0f);
			natives::SetPedKeepTask(c, TRUE);
			g_squad.push_back(c);
		}
		return true;
	}

	void squad_clear()
	{
		for (Ped c : g_squad)
			if (natives::DoesEntityExist(c))
				natives::DeletePed(&c);
		for (Vehicle v : g_squadCars)
			if (natives::DoesEntityExist(v))
				natives::DeleteEntity(&v);
		g_squad.clear();
		g_squadCars.clear();
		g_copTarget.clear();
		g_pendingCops = 0;
	}

	void mobwar_clear_all()
	{
		for (auto &[id, m] : g_mobs)
			double_delete(m);
		g_mobs.clear();
		g_doublePeds.clear();
		g_pedsSent.clear();
		squad_clear();
	}

	/// Every frame: doubles follow their mobs and report damage; GTA's people go to Minecraft; cops get targets.
	void mobs_tick(Ped player)
	{
		if (g_pendingCops > 0 && squad_spawn_one(player))
			--g_pendingCops;
		if (g_mobs.empty())
			return;
		const int now = natives::GetGameTimer();
		if (now - g_mobsSeenAt > 3000)
		{
			// Minecraft stopped listing its mobs without an empty list (disconnect, pause): nothing to fight
			for (auto &[id, m] : g_mobs)
				double_delete(m);
			g_mobs.clear();
			return;
		}
		for (auto &[id, m] : g_mobs)
		{
			if (m.ped != 0 && (!natives::DoesEntityExist(m.ped) || natives::IsPedDeadOrDying(m.ped)))
			{
				// killed outright (a big GTA explosion): hurt the mob a lot; either way make a new double
				if (natives::DoesEntityExist(m.ped) && !near_recent_boom(m))
					sendf("{\"t\":\"mobdmg\",\"id\":%d,\"d\":40}", id);
				double_delete(m);
			}
			if (m.ped == 0)
			{
				if (g_doublePeds.size() < kMaxDoubles && (m.ped = double_create(m.x, m.y, m.z)) != 0)
				{
					m.health = natives::GetEntityHealth(m.ped);
					++g_statDoubles;
				}
				continue;
			}
			natives::SetEntityCoordsNoOffset(m.ped, m.x, m.y, m.z + 1.0f);
			if (g_doubleVis == 1)
				natives::SetEntityLocallyInvisible(m.ped);
			const int hp = natives::GetEntityHealth(m.ped);
			if (hp < m.health)
			{
				if (!near_recent_boom(m))
				{
					sendf("{\"t\":\"mobdmg\",\"id\":%d,\"d\":%.2f}", id, float(m.health - hp) * g_mobDmgScale);
					++g_statDmg;
				}
				natives::SetEntityHealth(m.ped, kDoubleHealth);
				m.health = natives::GetEntityHealth(m.ped);
			}
		}

		// GTA's people near the player, 20 times a second (their feet, Minecraft coordinates)
		const Vector3 me = natives::GetEntityCoords(player, TRUE);
		if (now >= g_nextPedsAt)
		{
			g_nextPedsAt = now + 50;
			int handles[256];
			const int n = worldGetAllPeds(handles, 256);
			const int playerGroup = natives::GetPlayerGroup(natives::PlayerId());
			std::string list;
			g_pedsSent.clear();
			for (int i = 0; i < n && g_pedsSent.size() < 48; ++i)
			{
				const Ped q = handles[i];
				if (q == player || g_doublePeds.count(q) || natives::IsPedDeadOrDying(q))
					continue;
				const int type = natives::GetPedType(q);
				if (type == 28)
					continue; // animals (birds would be hunted in the sky)
				if (g_huntCopsOnly && type != 6 && type != 27 && type != 29)
					continue;
				const Vector3 o = natives::GetEntityCoords(q, TRUE);
				const float dx = o.x - me.x, dy = o.y - me.y;
				if (dx * dx + dy * dy > 60.0f * 60.0f)
					continue;
				// the player's friends and mission partners aren't prey (a mission fails when they die)
				if (natives::IsPedAPlayer(q) || natives::IsPedGroupMember(q, playerGroup) || natives::GetRelationshipBetweenPeds(q, player) <= 2)
					continue;
				char e[96];
				snprintf(e, sizeof(e), "%s[%d,%.3f,%.3f,%.3f]", list.empty() ? "" : ",", q, o.x, o.z - 1.0f + g_yOffset, -o.y);
				list += e;
				g_pedsSent.insert(q);
			}
			g_ws.send("{\"t\":\"peds\",\"p\":[" + list + "]}");
		}

		// police (spawned or not) fight the nearest double; re-tasked only when that one is gone
		if (now >= g_nextCopTaskAt)
		{
			g_nextCopTaskAt = now + 1000;
			std::vector<Ped> cops;
			for (Ped c : g_squad)
				if (natives::DoesEntityExist(c) && !natives::IsPedDeadOrDying(c))
					cops.push_back(c);
			int handles[256];
			const int n = worldGetAllPeds(handles, 256);
			for (int i = 0; i < n; ++i)
			{
				const Ped q = handles[i];
				if (q == player || g_doublePeds.count(q) || natives::IsPedDeadOrDying(q) || std::find(cops.begin(), cops.end(), q) != cops.end())
					continue;
				const int type = natives::GetPedType(q);
				bool fighter = type == 6 || type == 27 || type == 29;
				if (!fighter && g_hell.on)
				{
					// in hell the local gangs fight too
					const Hash group = natives::GetPedRelationshipGroupHash(q);
					static const Hash gangs[] = {natives::GetHashKey("AMBIENT_GANG_FAMILY"), natives::GetHashKey("AMBIENT_GANG_BALLAS"),
						natives::GetHashKey("AMBIENT_GANG_MEXICAN"), natives::GetHashKey("AMBIENT_GANG_LOST")};
					fighter = std::find(std::begin(gangs), std::end(gangs), group) != std::end(gangs);
				}
				if (!fighter)
					continue;
				const Vector3 o = natives::GetEntityCoords(q, TRUE);
				if ((o.x - me.x) * (o.x - me.x) + (o.y - me.y) * (o.y - me.y) < 90.0f * 90.0f)
					cops.push_back(q);
			}
			// (cops gone, or dead, forgotten: the map only grew)
			for (auto it = g_copTarget.begin(); it != g_copTarget.end();)
				it = !natives::DoesEntityExist(it->first) || natives::IsPedDeadOrDying(it->first) ? g_copTarget.erase(it) : std::next(it);
			for (Ped c : cops)
			{
				auto current = g_copTarget.find(c);
				if (current != g_copTarget.end() && g_doublePeds.count(current->second))
					continue;
				const Vector3 o = natives::GetEntityCoords(c, TRUE);
				Ped best = 0;
				float bestD = 90.0f * 90.0f;
				for (const auto &[id, m] : g_mobs)
				{
					const float dx = m.x - o.x, dy = m.y - o.y, dd = dx * dx + dy * dy;
					if (m.ped != 0 && dd < bestD)
					{
						best = m.ped;
						bestD = dd;
					}
				}
				if (best != 0)
				{
					natives::TaskCombatPed(c, best);
					g_copTarget[c] = best;
				}
			}
		}
	}

	/// Small patches of ground under each mob and each spawned cop (beyond the player's own radius), so mobs can
	/// chase people anywhere nearby. Probes start just above the mob or cop, not the player.
	void sample_patches()
	{
		int budget = 120;
		std::string columns, water;
		auto patch = [&](float gx, float gy, float gz, int radius) {
			const int cx = int(std::floor(gx)), cz = int(std::floor(-gy));
			for (int dx = -radius; dx <= radius && budget > 0; ++dx)
				for (int dz = -radius; dz <= radius && budget > 0; ++dz)
				{
					const int x = cx + dx, z = cz + dz;
					const auto done = g_sampled.find(column_key(x, z));
					if (done != g_sampled.end() && std::fabs(done->second - gz) < 2.0f)
						continue;
					--budget;
					float groundZ = 0.0f;
					// (under GTA's water its bed, as sample_ground: the water over it is Minecraft's water, not a floor)
					if (!natives::GetGroundZFor3dCoord(x + 0.5f, -(z + 0.5f), gz + 1.2f, &groundZ, TRUE, FALSE) || groundZ == 0.0f)
						continue;
					g_sampled[column_key(x, z)] = gz;
					const int top = int(std::floor(groundZ + g_yOffset + 0.5f)) - 1;
					char entry[64];
					snprintf(entry, sizeof(entry), "%s%d,%d,%d,%d", columns.empty() ? "" : ",", x, z, top - kGroundDepth + 1, top);
					columns += entry;
					float surface = 0.0f;
					if (water_surface(x + 0.5f, -(z + 0.5f), groundZ, std::max(gz, groundZ) + 20.0f, surface) && surface > groundZ + 0.1f)
					{
						snprintf(entry, sizeof(entry), "%s%d,%d,%d,%d", water.empty() ? "" : ",", x, z, top + 1, int((surface + g_yOffset) * 256.0f));
						water += entry;
					}
				}
		};
		for (const auto &[id, m] : g_mobs)
			patch(m.x, m.y, m.z, 3);
		for (Ped c : g_squad)
			if (natives::DoesEntityExist(c))
			{
				const Vector3 o = natives::GetEntityCoords(c, TRUE);
				patch(o.x, o.y, o.z - 1.0f, 2);
			}
		if (!columns.empty())
			g_ws.send("{\"t\":\"ground\",\"c\":[" + columns + "]}");
		if (!water.empty())
			g_ws.send("{\"t\":\"gwater\",\"c\":[" + water + "]}");
	}

	/// A real blast (grenade, rocket, car, tank shell, propane...) within r of (x, y, z): not a hydrant's or a steam
	/// pipe's jet, smoke, a flare or an extinguisher (GTA counts those as explosions too, and a broken hydrant's lasts).
	bool blast_near(float x, float y, float z, float r)
	{
		for (const int type : {0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 15, 16, 17, 23, 25, 26, 27, 28, 29, 31, 32, 34, 36, 37, 38, 43, 47, 49, 59, 60, 70, 71, 72, 82})
			if (natives::IsExplosionInSphere(type, x, y, z, r))
				return true;
		return false;
	}

	/// Every frame: GTA's own camera shake off (turned into ours), ours decaying, the portal warp easing in and out.
	void screen_fx_tick()
	{
		const int64_t now = now_nanos();
		const float dt = g_fx.last == 0 ? 0.016f : std::clamp(float(now - g_fx.last) * 1e-9f, 0.0f, 0.1f);
		g_fx.last = now;
		g_fx.t += dt;
		if (natives::IsGameplayCamShaking() || natives::IsCinematicCamShaking())
			++g_fx.gtaShakeFrames;
		// GTA shaking its camera (its own explosions: a grenade, a car going up; a crash) shook only GTA's picture, not
		// Minecraft's, which came apart from it: the shake goes on the finished picture instead, both together
		const Vector3 cc = natives::GetFinalRenderedCamCoord();
		const bool boom = blast_near(cc.x, cc.y, cc.z, 60.0f);
		if (g_fx.suppress && (boom || natives::IsGameplayCamShaking() || natives::IsCinematicCamShaking()))
		{
			// GTA shook its camera (an explosion of its own, a crash): Minecraft's frame can't follow, so shake the
			// picture instead
			natives::StopGameplayCamShaking(TRUE);
			natives::StopCinematicCamShaking(TRUE);
			const Vector3 c = cc;
			const float strength = !boom ? 0.15f : blast_near(c.x, c.y, c.z, 18.0f) ? 0.9f : 0.45f;
			if (g_fx.shake < strength)
				shake_impulse(strength - g_fx.shake);
		}
		const int game = natives::GetGameTimer();
		const float floor = game < g_fx.rumbleUntil ? g_fx.rumble : 0.0f;
		g_fx.shake = std::max(floor, g_fx.shake * std::exp(-dt * 3.2f));
		const float a = g_fx.shake, t = g_fx.t;
		const float sx = a * 0.010f * (0.6f * std::sin(t * 57.1f) + 0.4f * std::sin(t * 31.7f + 1.3f));
		const float sy = a * 0.010f * (0.6f * std::sin(t * 49.3f + 0.7f) + 0.4f * std::sin(t * 27.9f + 2.1f));
		const float roll = a * 0.012f * std::sin(t * 41.9f + 0.4f);
		const float target = game < g_fx.warpPulseUntil ? 1.0f : g_fx.warpTarget;
		g_fx.warp += (target - g_fx.warp) * (1.0f - std::exp(-dt * (target > g_fx.warp ? 5.0f : 1.4f)));
		if (g_fx.warp < 0.002f && target == 0.0f)
			g_fx.warp = 0.0f;
		if (g_fx.screenShake)
			compositor::set_screen_fx(sx, sy, roll, g_fx.warp);
		else
			compositor::set_screen_fx(0.0f, 0.0f, 0.0f, g_fx.warp);
	}

	int64_t cell_key(int x, int y, int z)
	{
		return (int64_t(x & 0x1FFFFF) << 42) | (int64_t(y & 0x1FFFFF) << 21) | int64_t(z & 0x1FFFFF);
	}

	void cell_of(int64_t k, int &x, int &y, int &z)
	{
		auto unpack = [](int64_t v) {
			v &= 0x1FFFFF;
			return int(v & 0x100000 ? v - 0x200000 : v);
		};
		x = unpack(k >> 42);
		y = unpack(k >> 21);
		z = unpack(k);
	}

	/// GTA's point at the middle of Minecraft's cell (x, y, z).
	Vector3 cell_middle(int x, int y, int z)
	{
		Vector3 p = {};
		p.x = float(x) + 0.5f;
		p.y = -(float(z) + 0.5f);
		p.z = float(y) + 0.5f - g_yOffset;
		return p;
	}


	int64_t cluster_key(int kind, int x, int z)
	{
		return (int64_t(kind) << 60) | (int64_t((x >> 2) & 0x3FFFFFFF) << 30) | int64_t((z >> 2) & 0x3FFFFFFF);
	}

	/// The integers of a flat JSON array field ("key":[1,2,3,...]).
	void json_ints(const std::string &m, const char *key, std::vector<int> &out)
	{
		out.clear();
		const char *p = json_value(m, key);
		if (p == nullptr || *p != '[')
			return;
		++p;
		while (*p != '\0' && *p != ']')
		{
			char *end = nullptr;
			const long v = std::strtol(p, &end, 10);
			if (end == p)
			{
				++p;
				continue;
			}
			out.push_back(int(v));
			p = end;
		}
	}

	void hot_add(int x, int y, int z, int kind)
	{
		const int64_t k = cell_key(x, y, z);
		if (g_hot.count(k))
			return;
		g_hot[k] = kind;
		HotCluster &c = g_hotClusters[cluster_key(kind, x, z)];
		c.sx += x + 0.5f;
		c.sy += y + 0.5f;
		c.sz += z + 0.5f;
		++c.n;
		c.kind = kind;
	}

	void hot_remove(int x, int y, int z)
	{
		const auto it = g_hot.find(cell_key(x, y, z));
		if (it == g_hot.end())
			return;
		const auto c = g_hotClusters.find(cluster_key(it->second, x, z));
		if (c != g_hotClusters.end())
		{
			c->second.sx -= x + 0.5f;
			c->second.sy -= y + 0.5f;
			c->second.sz -= z + 0.5f;
			if (--c->second.n <= 0)
				g_hotClusters.erase(c);
		}
		g_hot.erase(it);
	}

	/// {"t":"hot","lava":[x,y,z,...],"fire":[...],"soul":[...],"clear":[...]}: Minecraft's hot blocks, as they change.
	void hot_message(const std::string &m)
	{
		std::vector<int> v;
		const char *kinds[3] = {"lava", "fire", "soul"};
		for (int k = 0; k < 3; ++k)
		{
			json_ints(m, kinds[k], v);
			for (size_t i = 0; i + 2 < v.size(); i += 3)
				hot_add(v[i], v[i + 1], v[i + 2], k);
		}
		json_ints(m, "clear", v);
		for (size_t i = 0; i + 2 < v.size(); i += 3)
			hot_remove(v[i], v[i + 1], v[i + 2]);
	}

	bool hot_at(int x, int y, int z)
	{
		return g_hot.count(cell_key(x, y, z)) != 0;
	}

	/// Whether Minecraft's fire, lava or magma is round GTA's point (x, y, z): within `reach` across, and from a cell
	/// under it up to `up` over it.
	bool hot_near(float x, float y, float z, float reach, float up)
	{
		const int x0 = int(std::floor(x - reach)), x1 = int(std::floor(x + reach)), z0 = int(std::floor(-y - reach)), z1 = int(std::floor(-y + reach));
		const int y0 = int(std::floor(z + g_yOffset)) - 1, y1 = int(std::floor(z + g_yOffset + up));
		for (int cx = x0; cx <= x1; ++cx)
			for (int cz = z0; cz <= z1; ++cz)
				for (int cy = y0; cy <= y1; ++cy)
					if (hot_at(cx, cy, cz))
						return true;
		return false;
	}

	// Minecraft's fire as GTA's own fire too (it spreads to grass, people and cars as GTA's fires do): cell -> GTA's fire
	std::unordered_map<int64_t, int> g_scriptFires;

	/// Minecraft's fire near the player: a GTA fire on each burning cell (where GTA has none already: one of GTA's own
	/// that Minecraft got as fire, or one already started), at most 24; gone with Minecraft's fire.
	void script_fires_tick(Ped player)
	{
		const Vector3 me = natives::GetEntityCoords(player, TRUE);
		for (auto it = g_scriptFires.begin(); it != g_scriptFires.end();)
		{
			const auto hot = g_hot.find(it->first);
			if (hot != g_hot.end() && hot->second != 0)
			{
				++it;
				continue;
			}
			natives::RemoveScriptFire(it->second);
			it = g_scriptFires.erase(it);
		}
		int started = 0;
		for (const auto &[key, kind] : g_hot)
		{
			if (kind == 0 || g_scriptFires.size() >= 24 || started >= 4)
				continue; // (lava is Minecraft's alone: it lights what's by it, hot_tick)
			if (g_scriptFires.count(key))
				continue;
			int x, y, z;
			cell_of(key, x, y, z);
			const Vector3 at = cell_middle(x, y, z);
			if ((at.x - me.x) * (at.x - me.x) + (at.y - me.y) * (at.y - me.y) > 60.0f * 60.0f || natives::GetNumberOfFiresInRange(at.x, at.y, at.z, 1.2f) > 0)
				continue;
			g_scriptFires[key] = natives::StartScriptFire(at.x, at.y, at.z - 0.5f, 2, FALSE);
			++started;
		}
	}

	void script_fires_clear()
	{
		for (const auto &[key, fire] : g_scriptFires)
			natives::RemoveScriptFire(fire);
		g_scriptFires.clear();
	}

	/// Five times a second: GTA's people and cars on (or in) Minecraft's lava, fire or magma catch fire.
	void dent_vehicle(Vehicle v, const Vector3 &at, float damage, float radius);
	std::unordered_map<Entity, int> g_melting; // street furniture melting in lava: gone at that time

	void hot_tick(Ped player)
	{
		const int now = natives::GetGameTimer();
		if (now < g_nextHotCheckAt)
			return;
		if (g_hot.empty())
		{
			if (!g_scriptFires.empty())
				script_fires_clear(); // (Minecraft's fire all out, or cleared)
			return;
		}
		g_nextHotCheckAt = now + 200;
		script_fires_tick(player);
		const Vector3 me = natives::GetEntityCoords(player, TRUE);
		int handles[256];
		const int n = worldGetAllPeds(handles, 256);
		for (int i = 0; i < n; ++i)
		{
			const Ped q = handles[i];
			if (q == player || g_doublePeds.count(q) || natives::IsEntityOnFire(q))
				continue;
			const Vector3 o = natives::GetEntityCoords(q, TRUE);
			if ((o.x - me.x) * (o.x - me.x) + (o.y - me.y) * (o.y - me.y) > 80.0f * 80.0f)
				continue;
			// in it, or right by it (anywhere round their body, feet to head: lying down too)
			if (hot_near(o.x, o.y, o.z - 1.0f, 0.5f, 2.0f))
				natives::StartEntityFire(q);
		}
		const int cars = worldGetAllVehicles(handles, 256);
		for (int i = 0; i < cars; ++i)
		{
			const Vehicle v = handles[i];
			const Vector3 o = natives::GetEntityCoords(v, TRUE);
			if ((o.x - me.x) * (o.x - me.x) + (o.y - me.y) * (o.y - me.y) > 80.0f * 80.0f)
				continue;
			// what's under and in it: across its whole footprint, from the ground up to its roof
			const float h = natives::GetEntityHeading(v) * 3.14159265f / 180.0f, fx = -std::sin(h), fy = std::cos(h), rx = fy, ry = -fx;
			int fire = 0, lava = 0;
			float lx = 0.0f, ly = 0.0f;
			Vector3 touched[15] = {}; // (where lava touches it)
			for (float along = -1.8f; along <= 1.81f; along += 0.9f)
				for (float side = -0.8f; side <= 0.81f; side += 0.8f)
				{
					const float px = o.x + fx * along + rx * side, py = o.y + fy * along + ry * side;
					const int cx = int(std::floor(px)), cz = int(std::floor(-py));
					for (int cy = int(std::floor(o.z + g_yOffset - 1.6f)); cy <= int(std::floor(o.z + g_yOffset + 0.5f)); ++cy)
					{
						const auto it = g_hot.find(cell_key(cx, cy, cz));
						if (it == g_hot.end())
							continue;
						if (it->second == 0)
						{
							if (lava < 15)
								touched[lava] = v3(px, py, float(cy) + 1.0f - g_yOffset);
							++lava;
							lx += px;
							ly += py;
						}
						else
							++fire;
						break;
					}
				}
			if (fire + lava == 0)
			{
				// right by it (against its side, its bumper): it catches all the same
				if (!natives::IsEntityOnFire(v) && hot_near(o.x, o.y, o.z - 0.6f, 1.6f, 1.6f))
					natives::StartEntityFire(v);
				continue;
			}
			// fire under it: it burns (the engine catches, as a wrecked car's does)
			if (!natives::IsEntityOnFire(v))
				natives::StartEntityFire(v);
			if (natives::GetVehicleEngineHealth(v) > 0.0f)
				natives::SetVehicleEngineHealth(v, std::min(natives::GetVehicleEngineHealth(v), -1.0f));
			if (lava == 0)
				continue;
			// lava melts it where it touches: the tyres in it pop (only those), the body caves in there, the engine dies, and
			// it's pushed back out of it
			for (int k = 0; k < std::min(lava, 15); ++k)
			{
				const int tyre = nearest_tyre(v, touched[k].x, touched[k].y, o.z - 0.3f, 1.1f);
				if (tyre >= 0 && natives::GetVehicleTyresCanBurst(v) && !natives::IsVehicleTyreBurst(v, tyre, TRUE))
					natives::SetVehicleTyreBurst(v, tyre, TRUE, 1000.0f);
			}
			natives::SetVehicleBodyHealth(v, std::max(0.0f, natives::GetVehicleBodyHealth(v) - 60.0f * float(lava)));
			natives::SetVehicleEngineHealth(v, std::max(-3900.0f, natives::GetVehicleEngineHealth(v) - 40.0f * float(lava)));
			{
				// (a different touched place each time, at the body's own height: under the floor it dented nothing seen)
				Vector3 at = touched[(now / 200) % std::min(lava, 15)];
				at.z = o.z;
				dent_vehicle(v, at, 400.0f, 0.9f);
			}
			lx /= float(lava);
			ly /= float(lava);
			const float ax = o.x - lx, ay = o.y - ly, al = std::sqrt(ax * ax + ay * ay);
			if (al > 0.05f)
				natives::ApplyForceToEntity(v, ax / al * 5.0f, ay / al * 5.0f, 0.3f);
			else
			{
				// in it all over: pushed along the way it was going (out the far side), or backwards
				const Vector3 vel = natives::GetEntityVelocity(v);
				const float sl = std::sqrt(vel.x * vel.x + vel.y * vel.y);
				natives::ApplyForceToEntity(v, sl > 0.3f ? vel.x / sl * 5.0f : -fx * 5.0f, sl > 0.3f ? vel.y / sl * 5.0f : -fy * 5.0f, 0.3f);
			}
		}
		// street furniture (signs, lamp posts, bins, benches) in lava: knocked loose, alight, and gone after a while
		const int objs = worldGetAllObjects(handles, 256);
		for (int i = 0; i < objs; ++i)
		{
			const Object ob = handles[i];
			// (not Minecraft's blocks, Steve's gun, what anyone carries, money and weapons lying about, or a mission's own)
			if (g_props.handles.count(ob) || g_melting.count(ob) || ob == g_gunFit.held)
				continue;
			const Vector3 o = natives::GetEntityCoords(ob, TRUE);
			if ((o.x - me.x) * (o.x - me.x) + (o.y - me.y) * (o.y - me.y) > 60.0f * 60.0f)
				continue;
			if (natives::IsEntityAttached(ob) || natives::IsObjectAPickup(ob) || natives::IsObjectAPortablePickup(ob) ||
				(natives::IsEntityAMissionEntity(ob) && !is_loose(ob)))
				continue;
			const int cx = int(std::floor(o.x)), cz = int(std::floor(-o.y));
			bool inLava = false;
			for (int cy = int(std::floor(o.z + g_yOffset - 1.0f)); cy <= int(std::floor(o.z + g_yOffset + 0.5f)) && !inLava; ++cy)
			{
				const auto it = g_hot.find(cell_key(cx, cy, cz));
				inLava = it != g_hot.end() && it->second == 0;
			}
			if (!inLava)
			{
				// by fire (or lava): alight, where it stands
				if (!natives::IsEntityOnFire(ob) && hot_near(o.x, o.y, o.z - 0.5f, 0.8f, 1.5f))
					natives::StartEntityFire(ob);
				continue;
			}
			// (the map's own swapped for the plugin's copy, which sinks and burns away)
			const Object melt = loosen(ob);
			if (melt == 0 || g_melting.count(melt))
				continue;
			natives::SetEntityDynamic(melt, TRUE);
			natives::ApplyForceToEntity(melt, 0.0f, 0.0f, -2.0f);
			natives::StartEntityFire(melt);
			g_melting[melt] = now + 4000;
		}
		for (auto it = g_melting.begin(); it != g_melting.end();)
		{
			if (now < it->second)
			{
				++it;
				continue;
			}
			Entity e = it->first;
			if (natives::DoesEntityExist(e) && movable_object(e))
			{
				loose_gone(e); // (the map's own stays hidden till the player is far off)
				natives::SetEntityAsMissionEntity(e);
				natives::DeleteEntity(&e);
			}
			it = g_melting.erase(it);
		}
	}

	/// {"t":"water","set":[x,y,z,fx,fz,...],"clear":[x,y,z,...]}: Minecraft's water as it changes (Minecraft
	/// coordinates; fx, fz: the way it flows, in thousandths).
	void water_message(const std::string &m)
	{
		std::vector<int> v;
		auto remove = [](int x, int y, int z) {
			const auto it = g_water.find(cell_key(x, y, z));
			if (it == g_water.end())
				return;
			g_water.erase(it);
			const auto c = g_waterClusters.find(cluster_key(0, x, z));
			if (c != g_waterClusters.end())
			{
				c->second.sx -= x + 0.5f;
				c->second.sy -= y + 0.5f;
				c->second.sz -= z + 0.5f;
				if (--c->second.n <= 0)
					g_waterClusters.erase(c);
			}
		};
		json_ints(m, "clear", v);
		for (size_t i = 0; i + 2 < v.size(); i += 3)
			remove(v[i], v[i + 1], v[i + 2]);
		json_ints(m, "set", v);
		for (size_t i = 0; i + 4 < v.size(); i += 5)
		{
			const int x = v[i], y = v[i + 1], z = v[i + 2];
			remove(x, y, z);
			g_water[cell_key(x, y, z)] = {short(v[i + 3]), short(v[i + 4])};
			HotCluster &c = g_waterClusters[cluster_key(0, x, z)];
			c.sx += x + 0.5f;
			c.sy += y + 0.5f;
			c.sz += z + 0.5f;
			++c.n;
		}
	}

	/// The flow of Minecraft's water at GTA feet position (x, y, z), or false if there's none there.
	bool water_at(float x, float y, float z, float &fx, float &fy)
	{
		const int cx = int(std::floor(x)), cz = int(std::floor(-y)), cy = int(std::floor(z + g_yOffset + 0.2f));
		for (int dy = 0; dy >= -1; --dy)
		{
			const auto it = g_water.find(cell_key(cx, cy + dy, cz));
			if (it != g_water.end())
			{
				fx = it->second.first / 1000.0f;
				fy = -it->second.second / 1000.0f;
				return true;
			}
		}
		return false;
	}

	/// Four times a second: GTA's fires in Minecraft's water go out (on the ground, on people, on cars), and GTA's
	/// people standing in flowing water are swept along with it.
	void water_tick(Ped player)
	{
		const int now = natives::GetGameTimer();
		if (g_water.empty())
			return;
		// the player (moved by GTA, not Minecraft: Minecraft's own water carries Steve) in flowing water: carried along,
		// as Minecraft's water carries Steve. Only on its feet on the ground: put there every frame mid-jump or mid-fall,
		// the jump or fall was lost
		const float dt = natives::GetFrameTime();
		if (!g_walk.on && !g_carFly.on && !natives::IsPedInAnyVehicle(player, FALSE) && !natives::IsPedRagdoll(player) &&
			!natives::IsEntityInAir(player) && !natives::IsPedJumping(player) && !natives::IsPedFalling(player) && !natives::IsPedClimbing(player))
		{
			const Vector3 o = natives::GetEntityCoords(player, TRUE);
			float fx = 0.0f, fy = 0.0f;
			if (water_at(o.x, o.y, o.z - 1.0f, fx, fy) && fx * fx + fy * fy >= 0.01f)
			{
				const float step = 1.4f * dt / std::sqrt(fx * fx + fy * fy);
				natives::SetEntityCoordsNoOffset(player, o.x + fx * step, o.y + fy * step, o.z);
			}
		}
		// cars and loose things in flowing water (found four times a second, below): eased every frame towards the
		// water's pace along its flow, as Minecraft's water carries a boat or an item (shoved four times a second, they
		// lurched along)
		for (const auto &[e, f] : g_waterCarried)
		{
			if (!natives::DoesEntityExist(e))
				continue;
			const Vector3 v = natives::GetEntityVelocity(e);
			const float pace = f.car ? 2.5f : 2.0f, along = v.x * f.x + v.y * f.y;
			if (along < pace)
			{
				const float add = std::min(pace - along, (f.car ? 3.0f : 5.0f) * dt);
				natives::SetEntityVelocity(e, v.x + f.x * add, v.y + f.y * add, v.z);
			}
		}
		if (now < g_nextWaterAt)
			return;
		g_waterCarried.clear();
		g_nextWaterAt = now + 250;
		const Vector3 me = natives::GetEntityCoords(player, TRUE);
		for (const auto &[k, c] : g_waterClusters)
		{
			const float x = c.sx / c.n, y = -(c.sz / c.n), z = c.sy / c.n - g_yOffset;
			if ((x - me.x) * (x - me.x) + (y - me.y) * (y - me.y) < 90.0f * 90.0f)
				natives::StopFireInRange(x, y, z, 3.0f);
		}
		// GTA's people with their heads in Minecraft's water: drowning, as in Minecraft (15 s of breath, then hurt
		// every half second till they're out or dead)
		static std::unordered_map<Ped, int> underSince;
		for (auto it = underSince.begin(); it != underSince.end();)
			it = !natives::DoesEntityExist(it->first) || now - it->second > 120000 ? underSince.erase(it) : std::next(it);
		int handles[256];
		const int n = worldGetAllPeds(handles, 256);
		for (int i = 0; i < n; ++i)
		{
			const Ped q = handles[i];
			if (g_doublePeds.count(q) || natives::IsPedDeadOrDying(q))
				continue;
			const Vector3 o = natives::GetEntityCoords(q, TRUE);
			if ((o.x - me.x) * (o.x - me.x) + (o.y - me.y) * (o.y - me.y) > 80.0f * 80.0f)
				continue;
			if (q != player && !natives::IsPedInAnyVehicle(q, FALSE))
			{
				const Vector3 head = natives::GetPedBoneCoords(q, 31086);
				const bool under = g_water.count(cell_key(int(std::floor(head.x)), int(std::floor(head.z + g_yOffset)), int(std::floor(-head.y)))) != 0;
				const auto it = underSince.find(q);
				if (!under)
				{
					if (it != underSince.end())
						underSince.erase(it);
				}
				else if (it == underSince.end())
					underSince[q] = now;
				else if (now - it->second > 15000 && (now / 250) % 2 == 0)
				{
					natives::ApplyDamageToPed(q, 12);
					if (!natives::IsPedRagdoll(q))
						natives::SetPedToRagdoll(q, 1500); // (struggling)
				}
			}
			float fx = 0.0f, fy = 0.0f;
			if (!water_at(o.x, o.y, o.z - 1.0f, fx, fy))
				continue;
			if (natives::IsEntityOnFire(q))
				natives::StopEntityFire(q);
			if (q == player || fx * fx + fy * fy < 0.01f || natives::IsPedInAnyVehicle(q, FALSE))
				continue;
			// swept off their feet and carried along
			const float len = std::sqrt(fx * fx + fy * fy);
			natives::SetPedToRagdoll(q, 1200);
			natives::ApplyForceToEntity(q, fx / len * 7.0f, fy / len * 7.0f, 0.6f);
		}
		const int cars = worldGetAllVehicles(handles, 256);
		for (int i = 0; i < cars; ++i)
		{
			const Vector3 o = natives::GetEntityCoords(handles[i], TRUE);
			if ((o.x - me.x) * (o.x - me.x) + (o.y - me.y) * (o.y - me.y) > 80.0f * 80.0f)
				continue;
			float fx = 0.0f, fy = 0.0f;
			if (!water_at(o.x, o.y, o.z - 0.6f, fx, fy) && !water_at(o.x, o.y, o.z + 0.4f, fx, fy))
				continue;
			// doused: a burning engine doesn't flare up again (a wreck stays a wreck: water never mended one)
			const bool burning = natives::IsEntityOnFire(handles[i]) != FALSE;
			if (burning)
				natives::StopEntityFire(handles[i]);
			if (burning && !natives::IsEntityDead(handles[i]) && natives::GetVehicleEngineHealth(handles[i]) < 50.0f &&
				natives::GetVehicleEngineHealth(handles[i]) > -900.0f)
				natives::SetVehicleEngineHealth(handles[i], 50.0f);
			// carried along where it flows, as Minecraft's water carries a boat
			const float len = std::sqrt(fx * fx + fy * fy);
			if (len > 0.1f)
				g_waterCarried.push_back({handles[i], {fx / len, fy / len, true}});
		}
		// GTA's loose things (bins, cones, boxes, what was knocked over) carried along too, as Minecraft's items are
		const int objs = worldGetAllObjects(handles, 256);
		for (int i = 0; i < objs; ++i)
		{
			const Object ob = handles[i];
			// (not Minecraft's blocks, Steve's gun, what anyone carries, or a mission's own)
			if (g_props.handles.count(ob) || ob == g_gunFit.held || natives::IsEntityAttached(ob) ||
				(natives::IsEntityAMissionEntity(ob) && !is_loose(ob)))
				continue;
			const Vector3 o = natives::GetEntityCoords(ob, TRUE);
			if ((o.x - me.x) * (o.x - me.x) + (o.y - me.y) * (o.y - me.y) > 80.0f * 80.0f)
				continue;
			float fx = 0.0f, fy = 0.0f;
			if (!water_at(o.x, o.y, o.z, fx, fy) && !water_at(o.x, o.y, o.z + 0.5f, fx, fy))
				continue;
			const float len = std::sqrt(fx * fx + fy * fy);
			if (len > 0.1f)
				g_waterCarried.push_back({ob, {fx / len, fy / len, false}});
		}
	}

	/// Every frame: Minecraft's lava and fire light GTA's world around them (the nearest few dozen groups), and the
	/// open portal glows purple.
	void hell_lights(Ped player)
	{
		if (g_hell.on)
			natives::DrawLightWithRange(g_hell.x, g_hell.y, g_hell.z + 1.8f, 150, 50, 255, 11.0f, 7.0f);
		if (g_hotClusters.empty())
			return;
		const Vector3 me = natives::GetEntityCoords(player, TRUE);
		std::vector<std::pair<float, const HotCluster *>> nearby;
		nearby.reserve(g_hotClusters.size());
		for (const auto &[k, c] : g_hotClusters)
		{
			const float x = c.sx / c.n, y = -(c.sz / c.n);
			const float dd = (x - me.x) * (x - me.x) + (y - me.y) * (y - me.y);
			if (dd < 70.0f * 70.0f)
				nearby.emplace_back(dd, &c);
		}
		const size_t count = std::min<size_t>(nearby.size(), 40);
		std::partial_sort(nearby.begin(), nearby.begin() + count, nearby.end(), [](auto &a, auto &b) { return a.first < b.first; });
		for (size_t i = 0; i < count; ++i)
		{
			const HotCluster &c = *nearby[i].second;
			const float x = c.sx / c.n, y = -(c.sz / c.n), z = c.sy / c.n - g_yOffset + 1.0f;
			const float strength = std::min(1.0f, 0.35f + c.n / 10.0f);
			if (c.kind == 2)
				natives::DrawLightWithRange(x, y, z, 70, 170, 255, 5.0f, 3.0f * strength);
			else if (c.kind == 1)
				natives::DrawLightWithRange(x, y, z, 255, 140, 40, 5.0f, 3.5f * strength);
			else
				natives::DrawLightWithRange(x, y, z, 255, 95, 20, 6.5f, 4.5f * strength);
		}
	}

	/// The Nether opened at `pos` (Minecraft coordinates, the portal's bottom centre): hell in GTA's world too.
	void hell_start(const std::string &m)
	{
		const char *pos = json_value(m, "pos");
		double x = 0, y = 0, z = 0;
		if (pos == nullptr || sscanf_s(pos, "[%lf ,%lf ,%lf ]", &x, &y, &z) != 3)
			return;
		if (g_hell.on)
		{
			g_hell.x = float(x); // told again (a resync): same show, keep going
			g_hell.y = float(-z);
			g_hell.z = float(y) - g_yOffset;
			return;
		}
		g_hell.on = true;
		g_hell.t0 = natives::GetGameTimer();
		g_hell.x = float(x);
		g_hell.y = float(-z);
		g_hell.z = float(y) - g_yOffset;
		g_hell.fromMinutes = natives::GetClockHours() * 60 + natives::GetClockMinutes();
		if (g_realmClock < 0)
			g_realmClock = g_hell.fromMinutes;
		g_hell.locked = false;
		g_hell.nextCopsAt = g_hell.t0 + 7000;
		// the city turns: a red sky and grade fading in while the clock races to midnight, and the ground shakes
		natives::ClearOverrideWeather();
		natives::SetWeatherTypeOvertimePersist("HALLOWEEN", 5.0f);
		natives::SetTransitionTimecycleModifier("damage", 5.0f);
		natives::AnimpostfxPlay("ExplosionJosh3", 0, FALSE);
		natives::ShakeGameplayCam("LARGE_EXPLOSION_SHAKE", 0.25f);
		natives::ShakeGameplayCam("ROAD_VIBRATION_SHAKE", 0.7f);
		g_hell.shaking = true;
		g_fx.warpPulseUntil = g_hell.t0 + 1200;
		// Minecraft's side is lit by its own night now: don't darken it twice; show its ground over GTA's
		compositor::set_look(0.3f, 0.3f, 12.0f);
		// everyone is prey, and the police and the local gangs fight back
		g_huntCopsOnly = false;
		mob_group_init();
		for (const char *gang : {"AMBIENT_GANG_FAMILY", "AMBIENT_GANG_BALLAS", "AMBIENT_GANG_MEXICAN", "AMBIENT_GANG_LOST"})
			natives::SetRelationshipBetweenGroups(5, natives::GetHashKey(gang), g_mobGroup);
		natives::Notify("~r~The Nether is here");
	}

	/// Both realms closed: GTA's own weather, grade and clock again (the time it was when the first one opened).
	void realm_restore()
	{
		if (g_hell.on || g_end.on)
			return;
		natives::ClearTimecycleModifier();
		natives::ClearOverrideWeather();
		natives::ClearWeatherTypePersist();
		if (g_realmClock >= 0)
			natives::SetClockTime(g_realmClock / 60, g_realmClock % 60, 0);
		natives::PauseClock(FALSE);
		g_realmClock = -1;
		g_fx.rumbleUntil = 0;
		natives::StopGameplayCamShaking(TRUE);
		compositor::set_look(-1.0f, -1.0f, -1.0f);
		squad_clear();
	}

	void hell_stop()
	{
		if (!g_hell.on)
			return;
		g_hell.on = false;
		g_hot.clear();
		g_hotClusters.clear();
		realm_restore();
		natives::Notify("~g~You left the Nether");
	}

	/// The End opened at `pos` (Minecraft coordinates, the portal's centre): midnight and fog come over the city,
	/// alien colours flash, and the portal glows purple. Minecraft brings the endermen and the dragon.
	void end_start(const std::string &m)
	{
		const char *pos = json_value(m, "pos");
		double x = 0, y = 0, z = 0;
		if (pos == nullptr || sscanf_s(pos, "[%lf ,%lf ,%lf ]", &x, &y, &z) != 3)
			return;
		g_end.x = float(x);
		g_end.y = float(-z);
		g_end.z = float(y) - g_yOffset;
		if (g_end.on)
			return; // told again (a resync)
		g_end.on = true;
		g_end.t0 = natives::GetGameTimer();
		g_end.fromMinutes = natives::GetClockHours() * 60 + natives::GetClockMinutes();
		if (g_realmClock < 0)
			g_realmClock = g_end.fromMinutes;
		g_end.locked = false;
		natives::ClearOverrideWeather();
		natives::SetWeatherTypeOvertimePersist("FOGGY", 5.0f);
		natives::AnimpostfxPlay("DrugsMichaelAliensFightIn", 0, FALSE);
		natives::ShakeGameplayCam("LARGE_EXPLOSION_SHAKE", 0.15f);
		g_fx.warpPulseUntil = g_end.t0 + 1500;
		g_huntCopsOnly = false;
		mob_group_init();
		natives::Notify("~p~The End is here~s~ (walk into the portal again to leave)");
	}

	void end_stop()
	{
		if (!g_end.on)
			return;
		g_end.on = false;
		natives::AnimpostfxStop("DrugsMichaelAliensFightIn");
		realm_restore();
		natives::Notify("~g~You left the End");
	}

	/// Every frame while the End is open: the clock's race to midnight, then fog locked in, the portal's glow.
	void end_tick()
	{
		if (!g_end.on)
			return;
		const int el = natives::GetGameTimer() - g_end.t0;
		if (el <= 5200)
		{
			float k = std::clamp(el / 5000.0f, 0.0f, 1.0f);
			k = k * k * (3.0f - 2.0f * k);
			int to = 24 * 60;
			if (to < g_end.fromMinutes)
				to += 24 * 60;
			const int minutes = (g_end.fromMinutes + int((to - g_end.fromMinutes) * k)) % (24 * 60);
			natives::SetClockTime(minutes / 60, minutes % 60, 0);
			natives::PauseClock(TRUE);
		}
		else if (!g_end.locked)
		{
			natives::SetOverrideWeather("FOGGY");
			g_end.locked = true;
		}
		natives::DrawLightWithRange(g_end.x, g_end.y, g_end.z + 1.0f, 120, 40, 255, 12.0f, 6.0f);
	}

	/// Every frame while hell is on: the clock's race to midnight, the weather locking in, the police arriving.
	void hell_tick(Ped player)
	{
		if (!g_hell.on)
			return;
		const int now = natives::GetGameTimer(), el = now - g_hell.t0;
		if (el <= 5200)
		{
			float k = std::clamp(el / 5000.0f, 0.0f, 1.0f);
			k = k * k * (3.0f - 2.0f * k);
			int to = 23 * 60 + 40;
			if (to < g_hell.fromMinutes)
				to += 24 * 60;
			const int minutes = (g_hell.fromMinutes + int((to - g_hell.fromMinutes) * k)) % (24 * 60);
			natives::SetClockTime(minutes / 60, minutes % 60, 0);
			natives::PauseClock(TRUE);
		}
		else
		{
			if (!g_hell.locked)
			{
				natives::SetOverrideWeather("HALLOWEEN");
				g_hell.locked = true;
			}
			// GTA clears the "damage" grade itself (it's its hurt-player look): keep it on
			natives::SetTimecycleModifier("damage");
			natives::SetTimecycleModifierStrength(1.0f);
		}
		if (g_hell.shaking && el > 8000)
		{
			natives::StopGameplayCamShaking(FALSE);
			g_hell.shaking = false;
		}
		if (now >= g_hell.nextCopsAt && g_squad.size() < 12)
		{
			// the police respond (on the roads around), and keep coming
			g_copsLine = false;
			g_copsDist = 38.0f;
			g_pendingCops += g_squad.empty() ? 3 : 2;
			g_hell.nextCopsAt = now + 40000;
		}
	}

	/// How a Minecraft weapon's swing lands in GTA: reach (m), how wide (cosine of the half angle), damage, how hard
	/// people and cars are pushed (forward, up), how long people stay down, and whether car windows break.
	struct MeleeStyle
	{
		const char *kind;
		float reach, cone, damage, ped, pedUp, car, carUp;
		int ragdoll;   // ms a hit knocks people down for (0: a shove, as Minecraft's own knockback; a knockback
		               // enchantment knocks them down all the same)
		bool windows;
		float dent, dentRadius; // SET_VEHICLE_DAMAGE where it lands on a car
	};
	MeleeStyle g_meleeStyles[] = {
		{"fist", 2.3f, 0.5f, 25.0f, 3.0f, 1.0f, 0.0f, 0.0f, 700, false, 120.0f, 0.35f},
		{"tool", 2.8f, 0.4f, 60.0f, 5.0f, 2.0f, 6.0f, 3.0f, 1500, false, 200.0f, 0.4f},
		// the sword: Minecraft's sword, a shove (a fifth harder than before), no knocking down without knockback
		{"sword", 4.0f, 0.3f, 400.0f, 14.4f, 7.2f, 30.0f, 14.4f, 0, true, 300.0f, 0.5f},
		{"axe", 3.6f, 0.3f, 600.0f, 10.0f, 5.0f, 35.0f, 14.0f, 4000, true, 650.0f, 0.65f},
		{"trident", 4.2f, 0.3f, 500.0f, 13.0f, 6.0f, 28.0f, 12.0f, 4000, true, 350.0f, 0.5f},
		{"spear", 6.0f, 0.75f, 380.0f, 18.0f, 4.0f, 22.0f, 6.0f, 3500, true, 300.0f, 0.45f},
		{"mace", 3.2f, 0.2f, 450.0f, 14.0f, 9.0f, 40.0f, 18.0f, 4500, true, 800.0f, 0.8f},
	};
	float g_meleeRange = 8.0f; // what the crosshair is on is hit this far off (Minecraft's reach, /range)

	/// The enchantments on what Steve swings or shoots with (Minecraft says): knockback, sharpness, fire aspect, looting;
	/// for arrows power, punch, flame.
	struct Enchants
	{
		int knockback = 0, sharpness = 0, fire = 0, looting = 0, power = 0, punch = 0;
	};

	// GTA's people Minecraft's player hurt (a swing, an arrow, a trident): till when their death counts as its kill (a
	// hit marker for it, and experience orbs where they fell)
	std::unordered_map<Ped, int> g_mcVictims;

	/// Minecraft's swing or shot landed on GTA's person or car `e`: Minecraft shows a hit marker (and plays its sound).
	void mc_hit(Entity e, bool shot)
	{
		if (e == 0 || !natives::DoesEntityExist(e))
			return;
		const int type = natives::GetEntityType(e);
		if (type != 1 && type != 2)
			return;
		if (type == 1 && natives::IsPedDeadOrDying(e))
			return; // (a body shoved about)
		sendf("{\"t\":\"hitmark\",\"kill\":false,\"shot\":%s}", shot ? "true" : "false");
		if (type == 1)
			g_mcVictims[Ped(e)] = natives::GetGameTimer() + 8000;
	}

	/// Now and then: who Minecraft's player hurt and has since died is its kill: the kill marker, and experience
	/// (a person 5, a cop or soldier 8, an animal 2) where they lie.
	void victims_tick()
	{
		static int next = 0;
		const int now = natives::GetGameTimer();
		if (g_mcVictims.empty() || now < next)
			return;
		next = now + 100;
		for (auto it = g_mcVictims.begin(); it != g_mcVictims.end();)
		{
			const Ped q = it->first;
			if (!natives::DoesEntityExist(q) || now > it->second)
			{
				it = g_mcVictims.erase(it);
				continue;
			}
			if (!natives::IsPedDeadOrDying(q))
			{
				++it;
				continue;
			}
			const Vector3 o = natives::GetEntityCoords(q, TRUE);
			const int type = natives::GetPedType(q);
			const int xp = !natives::IsPedHuman(q) ? 2 : type == 6 || type == 27 || type == 29 ? 8 : 5;
			sendf("{\"t\":\"hitmark\",\"kill\":true,\"shot\":false}");
			sendf("{\"t\":\"xp\",\"pos\":[%.2f,%.2f,%.2f],\"n\":%d}", o.x, o.z - 1.0f + g_yOffset, -o.y, xp);
			it = g_mcVictims.erase(it);
		}
	}

	/// A dent where a swing (or anything) lands on car `v` at `at` (world), deeper for harder hits.
	/// A dent in car `v` at `at` (world), `radius` metres across. (SET_VEHICLE_DAMAGE's radius isn't in metres: about
	/// 250 to the metre; at the metre sizes it took, and the damage it had, the dents didn't show.)
	void dent_vehicle(Vehicle v, const Vector3 &at, float damage, float radius)
	{
		natives::SetVehicleCanBeVisiblyDamaged(v, TRUE);
		Vector3 local = natives::GetOffsetFromEntityGivenWorldCoords(v, at.x, at.y, at.z);
		// a point in the middle (no surface found) dents nothing: out to the body's side toward it
		const float flat = std::sqrt(local.x * local.x + local.y * local.y);
		if (flat < 0.6f)
		{
			local.x = flat > 1e-3f ? local.x / flat * 0.9f : 0.9f;
			local.y = flat > 1e-3f ? local.y / flat * 0.9f : 0.0f;
		}
		natives::SetVehicleDamage(v, local.x, local.y, local.z, damage * 3.0f, radius * 250.0f, TRUE);
	}

	/// Where on car `v` a swing from (x, y, z) lands: the nearest point of its body toward its middle (or the point
	/// itself if nothing is in the way).
	Vector3 vehicle_hit_point(Ped ped, Vehicle v, float x, float y, float z)
	{
		const Vector3 c = natives::GetEntityCoords(v, TRUE);
		BOOL hit = FALSE;
		Vector3 end = {}, normal = {};
		Entity e = 0;
		const int probe = natives::StartShapeTestLosProbe(x, y, z, c.x, c.y, c.z, 2, ped, 7);
		if (natives::GetShapeTestResult(probe, &hit, &end, &normal, &e) == 2 && hit && e == v)
			return end;
		return c;
	}

	/// A swing in Minecraft (`kind`: fist, tool, sword, axe, trident, spear, mace; `strength` 0..1 from Minecraft's
	/// attack cooldown; the weapon's enchantments): everyone in front of Steve (and whoever the crosshair is on, up to
	/// 8 m off) is hit: hurt, shoved or knocked down, set alight by fire aspect, carrying more money with looting; the
	/// dead are shoved about too. Cars within reach get a dent where the swing lands, a shove and a window smashed (all
	/// of them by a weapon), and glass in reach shatters, even to a fist. A mace swung while falling is a smash all
	/// round, and bounces the player.
	void melee(Ped ped, const std::string &kind, float strength, const Enchants &en)
	{
		const MeleeStyle *style = &g_meleeStyles[2];
		for (const MeleeStyle &s : g_meleeStyles)
			if (kind == s.kind)
				style = &s;
		const float s = std::clamp(strength, 0.0f, 1.0f);
		const float hurt = (0.2f + 0.8f * s * s) * (1.0f + 0.25f * float(en.sharpness)), push = std::max(0.3f, s);
		const float knock = 1.0f + 0.8f * float(std::min(en.knockback, 2000)); // (/enchant goes past 255: a launch, not a physics blow-up)
		const Vector3 me = natives::GetEntityCoords(ped, TRUE);
		const Vector3 cam = natives::GetFinalRenderedCamRot(2); // (Minecraft's free look renders with a camera of its own)
		const Vector3 eye = natives::GetFinalRenderedCamCoord();
		const float h = cam.z * 3.14159265f / 180.0f, pt = cam.x * 3.14159265f / 180.0f;
		const float fx = -std::sin(h), fy = std::cos(h);
		const float ux = fx * std::cos(pt), uy = fy * std::cos(pt), uz = std::sin(pt);
		const bool smash = kind == "mace" && natives::GetEntityVelocity(ped).z < -4.0f;
		const float reach = smash ? 6.0f : style->reach * std::max(1.0f, g_meleeRange / 8.0f), cone = smash ? -1.0f : style->cone;
		g_doing = "Minecraft's melee: what the crosshair is on";
		// what the crosshair is on, from beside Steve up to 8 m on (a person, a body, a car)
		const float along = std::max(0.0f, (me.x - eye.x) * ux + (me.y - eye.y) * uy + (me.z + 0.5f - eye.z) * uz);
		const float ax = eye.x + ux * along, ay = eye.y + uy * along, az = eye.z + uz * along;
		Entity aimed = 0, aimedObject = 0;
		Vector3 aimedAt = {};
		{
			BOOL hit = FALSE;
			Vector3 normal = {};
			const int probe = natives::StartShapeTestLosProbe(ax, ay, az, ax + ux * g_meleeRange, ay + uy * g_meleeRange, az + uz * g_meleeRange,
				1 | 2 | 4 | 8 | 16, ped, 7);
			if (natives::GetShapeTestResult(probe, &hit, &aimedAt, &normal, &aimed) != 2 || !hit || g_doublePeds.count(aimed) ||
				g_props.handles.count(aimed))
			{
				// (one of Minecraft's own blocks isn't a wall here: a body lying in placed blocks can still be hit)
				if (hit && aimed != 0 && g_props.handles.count(aimed))
					hit = FALSE;
				aimed = 0;
			}
			if (aimed != 0 && !natives::DoesEntityExist(aimed))
				aimed = 0; // (the map itself: no entity of its own)
			if (aimed != 0 && natives::GetEntityType(aimed) == 3)
			{
				aimedObject = aimed; // (a loose thing: a lamp post, a sign, a car's bumper or door lying about)
				aimed = 0;
			}
			if (aimed == 0 && aimedObject == 0)
			{
				// a thin thing just off the crosshair's line (a sign's pole, a lamp post): a fatter line finds it
				BOOL fat = FALSE;
				Vector3 fatAt = {}, fatN = {};
				Entity fatE = 0;
				const float span = hit ? std::sqrt((aimedAt.x - ax) * (aimedAt.x - ax) + (aimedAt.y - ay) * (aimedAt.y - ay) +
					(aimedAt.z - az) * (aimedAt.z - az)) + 0.3f : g_meleeRange;
				const int capsule = natives::StartShapeTestCapsule(ax, ay, az, ax + ux * span, ay + uy * span, az + uz * span, 0.3f, 16, ped, 7);
				if (natives::GetShapeTestResult(capsule, &fat, &fatAt, &fatN, &fatE) == 2 && fat && fatE != 0 && natives::DoesEntityExist(fatE) &&
					natives::GetEntityType(fatE) == 3 && !g_props.handles.count(fatE) && !natives::IsEntityAttached(fatE))
					aimedObject = fatE;
			}
			if (aimed == 0 && aimedObject == 0)
			{
				// the crosshair just past someone (a person is thin, and far off GTA gives them no collision for a line):
				// whoever is closest to its line, within a hand's breadth
				float best = 0.45f;
				int handles[256];
				const int n = worldGetAllPeds(handles, 256);
				const float limit = hit ? std::sqrt((aimedAt.x - ax) * (aimedAt.x - ax) + (aimedAt.y - ay) * (aimedAt.y - ay) + (aimedAt.z - az) * (aimedAt.z - az))
					: g_meleeRange;
				for (int i = 0; i < n; ++i)
				{
					const Ped q = handles[i];
					if (q == ped || g_doublePeds.count(q))
						continue;
					const Vector3 o = natives::GetEntityCoords(q, TRUE);
					const float px = o.x - ax, py = o.y - ay, pz = o.z + 0.2f - az;
					const float t = px * ux + py * uy + pz * uz;
					if (t < 0.0f || t > limit + 0.3f)
						continue;
					const float lx = px - ux * t, ly = py - uy * t, lz = (pz - uz * t) * 0.5f; // (a person is tall: up and down counts less)
					const float off = std::sqrt(lx * lx + ly * ly + lz * lz);
					if (off < best)
					{
						best = off;
						aimed = q;
						aimedAt = Vector3{o.x - lx, 0, o.y - ly, 0, o.z, 0};
					}
				}
				// (not through a wall: nothing of GTA's between the eye and them)
				if (aimed != 0)
				{
					Vector3 wallAt = {}, wallN = {};
					Entity wallE = 0;
					Hash wallM = 0;
					const Vector3 o = natives::GetEntityCoords(aimed, TRUE);
					if (world_probe(ped, ax, ay, az, o.x, o.y, o.z + 0.2f, 1 | 16, 7, wallAt, wallN, wallE, wallM) && wallE != aimed &&
						(wallE == 0 || !g_props.handles.count(wallE)))
						aimed = 0;
				}
			}
		}
		{
			const Entity on = aimed != 0 ? aimed : aimedObject;
			logf("melee %s (%.2f): on %d type %d model %08X", kind.c_str(), s, int(on), on != 0 ? natives::GetEntityType(on) : 0,
				on != 0 ? unsigned(natives::GetEntityModel(on)) : 0u);
		}
		auto hitPed = [&](Ped q, float dx, float dy, float d) {
			if (natives::IsPedDeadOrDying(q) || natives::IsPedFatallyInjured(q))
			{
				// a body: shoved along, however it lies (in the ground blocks or not)
				natives::ApplyForceToEntity(q, ux * 9.0f * push * knock, uy * 9.0f * push * knock, 3.0f * push + uz * 9.0f * push * knock);
				return;
			}
			mc_hit(q, false);
			if (en.looting > 0)
				natives::SetPedMoney(q, 40 * (1 + 2 * en.looting)); // (dropped when they die)
			const int damage = std::max(1, int(style->damage * hurt * (smash ? 2.0f : 1.0f)));
			if (en.fire > 0)
				natives::StartEntityFire(q);
			if (smash)
			{
				const float n = std::max(d, 0.3f);
				knock_ped(q, dx / n * 8.0f, dy / n * 8.0f, 16.0f, style->ragdoll, damage);
			}
			else if (style->ragdoll > 0 || en.knockback > 0)
			{
				// along the look: looking up while hitting sends them up. Flying first, hurt after (see knock_ped)
				const float f = style->ped * push * knock;
				knock_ped(q, ux * f, uy * f, style->pedUp * push * knock * (1.0f - 0.5f * std::fabs(uz)) + uz * f, std::max(style->ragdoll, 2500), damage);
			}
			else
			{
				natives::ApplyDamageToPed(q, damage);
				// Minecraft's knockback: a shove back and a little up, on their feet
				const Vector3 v = natives::GetEntityVelocity(q);
				const float k = style->ped / 12.0f * 3.0f * push;
				natives::SetEntityVelocity(q, v.x + ux * k, v.y + uy * k, v.z + style->pedUp / 6.0f * 1.6f * push + std::max(0.0f, uz) * k);
			}
		};
		g_doing = "Minecraft's melee: people hit";
		std::set<Entity> done;
		int handles[256];
		const int n = worldGetAllPeds(handles, 256);
		for (int i = 0; i < n; ++i)
		{
			const Ped q = handles[i];
			if (q == ped || g_doublePeds.count(q) || !smash)
				continue; // (only the mace's smash hits everyone round about; a swing hits what it's on, below)
			const Vector3 o = natives::GetEntityCoords(q, TRUE);
			const float dx = o.x - me.x, dy = o.y - me.y, d = std::sqrt(dx * dx + dy * dy);
			if (d > reach || std::fabs(o.z - me.z) > 2.5f || (d > 0.3f && (dx * fx + dy * fy) / d < cone))
				continue;
			hitPed(q, dx, dy, d);
			done.insert(q);
		}
		if (aimed != 0 && !done.count(aimed) && natives::GetEntityType(aimed) == 1)
		{
			const Vector3 o = natives::GetEntityCoords(aimed, TRUE);
			const float dx = o.x - me.x, dy = o.y - me.y;
			hitPed(Ped(aimed), dx, dy, std::sqrt(dx * dx + dy * dy));
			done.insert(aimed);
		}
		auto hitCar = [&](Vehicle v, const Vector3 &at) {
			mc_hit(v, false);
			if (style->car > 0.0f)
				natives::ApplyForceToEntity(v, ux * style->car * push * knock, uy * style->car * push * knock,
					style->carUp * push * (smash ? 2.0f : 1.0f) + uz * style->car * push * knock);
			// a dent where it lands: a fist a little one, an axe a deep one
			dent_vehicle(v, at, style->dent * (0.4f + 0.6f * s) * (1.0f + 0.25f * float(en.sharpness)) * (smash ? 2.0f : 1.0f), style->dentRadius);
			if (en.fire > 0 && s > 0.9f)
				natives::StartEntityFire(v);
			// a parked car hit: its alarm goes off
			if (natives::GetVehicleNumberOfPassengers(v) == 0 && !natives::IsVehicleAlarmActivated(v))
			{
				natives::SetVehicleAlarm(v, TRUE);
				natives::StartVehicleAlarm(v);
			}
			// glass where the swing lands only, as a bullet breaks it (not every window at once)
			{
				const Vector3 c = natives::GetEntityCoords(v, TRUE);
				const float wx = c.x - at.x, wy = c.y - at.y, wz = c.z - at.z, wl = std::max(0.1f, std::sqrt(wx * wx + wy * wy + wz * wz));
				Vector3 glassAt = at;
				glassAt.z += 0.3f;
				if (nearest_window(v, glassAt.x, glassAt.y, glassAt.z, 1.2f) >= 0)
					break_glass(ped, glassAt, wx / wl, wy / wl, wz / wl, v);
			}
		};
		g_doing = "Minecraft's melee: cars hit";
		each_vehicle_near(me.x, me.y, me.z, reach + 2.5f, [&](Vehicle v, float dx, float dy, float) {
			const float d = std::sqrt(dx * dx + dy * dy);
			const Vector3 o = natives::GetEntityCoords(v, TRUE);
			if (!smash || d > reach + 1.5f || std::fabs(o.z - me.z) > 3.0f || (d > 0.5f && (dx * fx + dy * fy) / d < std::min(cone, 0.2f)))
				return;
			hitCar(v, aimed == v ? aimedAt : vehicle_hit_point(ped, v, me.x, me.y, me.z + 0.3f));
			done.insert(v);
		});
		if (aimed != 0 && !done.count(aimed) && natives::GetEntityType(aimed) == 2)
			hitCar(Vehicle(aimed), aimedAt);
		{
			// street furniture (lamp posts, signs, bins, hydrants, benches) and loose things lying about (a car's bumper or
			// door): knocked loose and launched along the look, light things further, knockback much further
			auto fling = [&](Entity e) {
				g_doing = "Minecraft's melee: a thing knocked loose";
				// (the map's own swapped for the plugin's copy: GTA crashed when its own were woken or pushed)
				const float light = std::clamp(std::sqrt(60.0f / std::max(1.0f, entity_mass(e))), 0.35f, 1.5f);
				const float kb = 1.0f + float(std::min(en.knockback, 50));
				const float speed = std::min(60.0f, (3.0f + 7.0f * s) * light * kb * (smash ? 1.6f : 1.0f) * (style->windows ? 1.3f : 1.0f));
				const float up = (1.5f + 2.5f * s) * std::min(light, 1.0f) * (smash ? 1.6f : 1.0f);
				launch_object(e, ux * speed, uy * speed, up + uz * speed, 3000.0f);
			};
			// the one the crosshair is on, itself (looked for among the objects around, it often wasn't listed: a street
			// has hundreds, and Minecraft's placed blocks are some of them too)
			if (aimedObject != 0)
				fling(aimedObject);
			if (smash)
			{
				// the mace's smash: everything round about
				int objs[256];
				const int n = worldGetAllObjects(objs, 256); // (more gives that many of GTA's objects script handles: its pool of those runs out)
				for (int i = 0; i < n; ++i)
				{
					const Object ob = objs[i];
					if (Entity(ob) == aimedObject || g_props.handles.count(ob))
						continue;
					const Vector3 o = natives::GetEntityCoords(ob, TRUE);
					const float dx = o.x - me.x, dy = o.y - me.y, d = std::sqrt(dx * dx + dy * dy);
					if (d > reach + 0.8f || o.z < me.z - 1.5f || o.z > me.z + 2.5f)
						continue;
					fling(ob);
				}
			}
		}
		g_doing = "Minecraft's melee: glass";
		{
			// glass in reach along the crosshair's line, from beside Steve (shop windows, a car's windscreen): shattered
			const float span = reach + 0.5f;
			BOOL hit = FALSE;
			Vector3 end = {}, normal = {};
			Entity entity = 0;
			Hash material = 0;
			const int probe = natives::StartShapeTestLosProbe(ax, ay, az, ax + ux * span, ay + uy * span, az + uz * span, kGlassFlags, ped, kSeeGlass);
			if (natives::GetShapeTestResultIncludingMaterial(probe, &hit, &end, &normal, &material, &entity) == 2 && hit && breakable_glass(material))
				break_glass(ped, end, ux, uy, uz, entity);
		}
		if (smash)
		{
			// the mace's bounce: no fall, a hop back up
			const Vector3 v = natives::GetEntityVelocity(ped);
			natives::SetEntityVelocity(ped, v.x * 0.3f, v.y * 0.3f, 3.0f);
			shake_impulse(0.7f);
		}
		else if (!done.empty() && s > 0.8f && (style->windows || kind == "sword"))
			shake_impulse(kind == "sword" ? 0.05f : 0.12f); // (a heavy blow that lands: a little jolt)
	}

	/// Whether the director's armed flight has gone off its edge (dropped below where it was armed).
	bool drive_should_launch(Ped ped)
	{
		return natives::GetEntityCoords(ped, TRUE).z < g_drive.armZ - g_drive.armDrop;
	}

	/// Touched down: back to walking where Minecraft's player landed, facing the way it flew; armed again for the next jump.
	void drive_land(Ped ped, float x, float y, float groundZ)
	{
		drive_set(ped, false, true);
		natives::SetEntityCoordsNoOffset(ped, x, y, groundZ + 1.0f);
		natives::SetEntityHeading(ped, g_drive.sHeading);
		natives::SetGameplayCamRelativeHeading(0.0f);
		sendf("{\"t\":\"glide\",\"on\":false}");
		g_drive.armZ = groundZ + 1.0f;
		g_drive.armed = true;
		g_drive.armAfter = natives::GetGameTimer() + 1500;
	}

	/// Scripted shots, sent by a director script through Minecraft's link: {"t":"gta","op":...} in GTA coordinates.
	void handle_director(const std::string &m)
	{
		const Ped ped = natives::PlayerPedId();
		const std::string op = json_str(m, "op");
		const float x = float(json_num(m, "x", 0)), y = float(json_num(m, "y", 0)), z = float(json_num(m, "z", 0));
		if (op == "teleport")
		{
			natives::NewLoadSceneStartSphere(x, y, z, 80.0f);
			for (int i = 0; i < 300 && !natives::IsNewLoadSceneLoaded(); ++i)
				WAIT(0);
			natives::NewLoadSceneStop();
			natives::RequestCollisionAtCoord(x, y, z);
			natives::SetEntityCoordsNoOffset(ped, x, y, z);
			natives::SetEntityHeading(ped, float(json_num(m, "h", 0)));
			natives::SetGameplayCamRelativeHeading(0.0f);
			natives::SetGameplayCamRelativePitch(float(json_num(m, "pitch", 0)), 1.0f);
			g_haveOffset = false; // re-level the Minecraft ground here
		}
		else if (op == "walk")
			natives::TaskGoStraightToCoord(ped, x, y, z, float(json_num(m, "speed", 1.0)), int(json_num(m, "timeout", 20000)),
				float(json_num(m, "h", 40000.0)), 0.1f);
		else if (op == "stop")
			natives::ClearPedTasks(ped);
		else if (op == "face")
			natives::SetEntityHeading(ped, float(json_num(m, "h", 0)));
		else if (op == "view")
			natives::SetFollowPedCamViewMode(int(json_num(m, "mode", 1)));
		else if (op == "look")
		{
			natives::SetGameplayCamRelativeHeading(float(json_num(m, "heading", 0)));
			natives::SetGameplayCamRelativePitch(float(json_num(m, "pitch", 0)), 1.0f);
		}
		else if (op == "time")
		{
			natives::SetClockTime(int(json_num(m, "h", 12)), int(json_num(m, "m", 0)), 0);
			natives::PauseClock(TRUE);
		}
		else if (op == "weather")
		{
			const std::string w = json_str(m, "w");
			natives::SetWeatherTypeNowPersist(w.c_str());
			natives::SetOverrideWeather(w.c_str());
		}
		else if (op == "explode")
		{
			natives::AddExplosion(x, y, z, int(json_num(m, "type", 2)), float(json_num(m, "scale", 1.0)), TRUE, FALSE, 0.0f, FALSE);
			shake_from(x, y, z, 0.9f);
		}
		else if (op == "ped" || op == "car")
		{
			const std::string model = json_str(m, "model");
			const Hash hash = natives::GetHashKey(model.c_str());
			natives::RequestModel(hash);
			for (int i = 0; i < 200 && !natives::HasModelLoaded(hash); ++i)
				WAIT(0);
			if (natives::HasModelLoaded(hash))
			{
				if (op == "ped")
				{
					const Ped p = natives::CreatePed(ped_type(model), hash, x, y, z, float(json_num(m, "h", 0)));
					const std::string scenario = json_str(m, "scenario");
					const std::string dict = json_str(m, "anim_dict"), anim = json_str(m, "anim");
					if (!dict.empty() && !anim.empty())
					{
						natives::RequestAnimDict(dict.c_str());
						for (int i = 0; i < 200 && !natives::HasAnimDictLoaded(dict.c_str()); ++i)
							WAIT(0);
						natives::TaskPlayAnimLoop(p, dict.c_str(), anim.c_str());
					}
					else if (!scenario.empty())
						natives::TaskStartScenarioInPlace(p, scenario.c_str());
				}
				else
					natives::SetVehicleOnGroundProperly(natives::CreateVehicle(hash, x, y, z, float(json_num(m, "h", 0))));
				natives::SetModelAsNoLongerNeeded(hash);
			}
		}
		else if (op == "safe")
			make_safe(ped);
		else if (op == "relevel")
			g_haveOffset = false;
		else if (op == "fadein")
			natives::DoScreenFadeIn(500);
		else if (op == "drive")
		{
			g_drive.dist = float(json_num(m, "dist", g_drive.dist));
			g_drive.height = float(json_num(m, "height", g_drive.height));
			drive_set(ped, json_num(m, "on", 1.0) != 0.0);
		}
		else if (op == "gun")
			gun_set(ped, int(json_num(m, "w", -1)));
		else if (op == "police")
		{
			// a wanted level that stays: cops come (the player is invincible regardless)
			const Player player = natives::PlayerId();
			const int stars = int(json_num(m, "stars", 3));
			g_police = stars > 0;
			natives::SetMaxWantedLevel(g_police ? 5 : 0);
			natives::SetPoliceIgnorePlayer(player, g_police ? FALSE : TRUE);
			natives::SetDispatchCopsForPlayer(player, g_police ? TRUE : FALSE);
			if (g_police)
			{
				natives::SetPlayerWantedLevel(player, stars);
				natives::SetPlayerWantedLevelNow(player);
			}
			else
				natives::ClearPlayerWantedLevel(player);
		}
		else if (op == "army")
		{
			if (json_num(m, "clear", 0) != 0)
				army_clear();
			else
				army_spawn(ped, int(json_num(m, "tanks", 2)), int(json_num(m, "helis", 2)), float(json_num(m, "dist", 70)));
		}
		else if (op == "cops")
		{
			// police vs Minecraft's mobs: no wanted level (they leave the player alone), a squad arriving
			g_police = false;
			make_safe(ped);
			mob_group_init();
			g_pendingCops += int(json_num(m, "cars", 3));
			g_copsDist = float(json_num(m, "dist", 40));
			g_copsLine = json_num(m, "line", 0) != 0;
		}
		else if (op == "copsclear")
			squad_clear();
		else if (op == "mobfit")
		{
			g_doubleVis = int(json_num(m, "vis", g_doubleVis));
			g_huntCopsOnly = json_num(m, "copsonly", g_huntCopsOnly ? 1 : 0) != 0;
			g_mobHitScale = float(json_num(m, "hit", g_mobHitScale));
			g_mobDmgScale = float(json_num(m, "dmg", g_mobDmgScale));
			for (auto &[id, d] : g_mobs)
				if (d.ped != 0)
					double_visibility(d.ped);
		}
		else if (op == "poselag")
			compositor::set_pose_lag(int(json_num(m, "n", 1)));
		else if (op == "fx")
		{
			g_fx.screenShake = json_num(m, "shake", g_fx.screenShake ? 1 : 0) != 0;
			g_fx.suppress = json_num(m, "suppress", g_fx.suppress ? 1 : 0) != 0;
			sendf("{\"t\":\"gtainfo\",\"fx\":{\"shake\":%d,\"suppress\":%d,\"gtaShakeFrames\":%d}}",
				g_fx.screenShake ? 1 : 0, g_fx.suppress ? 1 : 0, g_fx.gtaShakeFrames);
			g_fx.gtaShakeFrames = 0;
		}
		else if (op == "hell")
		{
			// testing: hell on at the player (or at x, y, z), or off
			if (json_num(m, "on", 1) != 0)
			{
				const Vector3 me = natives::GetEntityCoords(ped, TRUE);
				char msg[160];
				snprintf(msg, sizeof(msg), "{\"pos\":[%.2f,%.2f,%.2f]}", me.x, me.z - 1.0f + g_yOffset, -me.y);
				hell_start(msg);
			}
			else
				hell_stop();
		}
		else if (op == "tcmod")
		{
			// debug/look-dev: a timecycle modifier (a GTA colour grade) by name, at a strength; "" clears it
			const std::string name = json_str(m, "name");
			if (name.empty())
				natives::ClearTimecycleModifier();
			else
			{
				natives::SetTimecycleModifier(name.c_str());
				natives::SetTimecycleModifierStrength(float(json_num(m, "s", 1.0)));
			}
		}
		else if (op == "postfx")
		{
			const std::string name = json_str(m, "name");
			if (json_num(m, "stop", 0) != 0)
				natives::AnimpostfxStop(name.c_str());
			else
				natives::AnimpostfxPlay(name.c_str(), int(json_num(m, "ms", 0)), json_num(m, "loop", 0) != 0);
		}
		else if (op == "mobwarclear")
			mobwar_clear_all();
		else if (op == "mobinfo")
		{
			// debug: doubles, their health, and what the cops are doing
			std::string info;
			for (const auto &[id, d] : g_mobs)
			{
				char e[128];
				snprintf(e, sizeof(e), "%s[%d,%d,%d,%.1f,%.1f,%.1f]", info.empty() ? "" : ",", id, d.ped, d.ped ? natives::GetEntityHealth(d.ped) : -1, d.x, d.y, d.z);
				info += e;
			}
			sendf("{\"t\":\"gtainfo\",\"mobs\":[%s],\"squad\":%d,\"targets\":%d,\"peds\":%d,\"hits\":%d,\"dmg\":%d,\"doubles\":%d}", info.c_str(),
				int(g_squad.size()), int(g_copTarget.size()), int(g_pedsSent.size()), g_statHits, g_statDmg, g_statDoubles);
		}
		else if (op == "meleefit")
		{
			// a weapon's swing ("k", default the sword)
			const std::string kind = json_str(m, "k");
			for (MeleeStyle &s : g_meleeStyles)
				if (kind == s.kind || (kind.empty() && std::strcmp(s.kind, "sword") == 0))
				{
					s.ped = float(json_num(m, "ped", s.ped));
					s.pedUp = float(json_num(m, "up", s.pedUp));
					s.car = float(json_num(m, "car", s.car));
					s.carUp = float(json_num(m, "carup", s.carUp));
					s.reach = float(json_num(m, "reach", s.reach));
					s.damage = float(json_num(m, "dmg", s.damage));
				}
		}
		else if (op == "gunfit")
		{
			g_gunFit.fwd = float(json_num(m, "fwd", g_gunFit.fwd));
			g_gunFit.right = float(json_num(m, "right", g_gunFit.right));
			g_gunFit.up = float(json_num(m, "up", g_gunFit.up));
			g_gunFit.yaw = float(json_num(m, "yaw", g_gunFit.yaw));
			g_gunFit.pitch = float(json_num(m, "pitch", g_gunFit.pitch));
			g_gunFit.roll = float(json_num(m, "roll", g_gunFit.roll));
			g_gunFit.tilt = float(json_num(m, "tilt", g_gunFit.tilt));
		}
		else if (op == "armdrive")
		{
			g_drive.dist = float(json_num(m, "dist", g_drive.dist));
			g_drive.height = float(json_num(m, "height", g_drive.height));
			g_drive.armHeading = float(json_num(m, "h", natives::GetEntityHeading(ped)));
			g_drive.armPitch = float(json_num(m, "p", -20.0));
			g_drive.armSpeed = float(json_num(m, "speed", 1.0));
			g_drive.armDrop = float(json_num(m, "drop", 0.6));
			g_drive.armHold = int(json_num(m, "hold", 3000));
			g_drive.user = json_num(m, "user", 0.0) != 0.0;
			g_drive.armZ = natives::GetEntityCoords(ped, TRUE).z;
			g_drive.armed = json_num(m, "on", 1.0) != 0.0;
		}
		else if (op == "probe")
		{
			// ground heights along a line: from (x, y) towards heading h, n points `step` metres apart, probing down from z
			const float h = float(json_num(m, "h", 0)) * 3.14159265f / 180.0f, step = float(json_num(m, "step", 1.0));
			const int n = int(json_num(m, "n", 20));
			std::string zs;
			for (int i = 0; i < n; ++i)
			{
				float g = -1000.0f;
				natives::GetGroundZFor3dCoord(x - std::sin(h) * step * i, y + std::cos(h) * step * i, z, &g, FALSE, FALSE);
				char e[24];
				snprintf(e, sizeof(e), "%s%.2f", zs.empty() ? "" : ",", g);
				zs += e;
			}
			sendf("{\"t\":\"gtainfo\",\"probe\":[%s]}", zs.c_str());
		}
		else if (op == "flylook")
		{
			g_drive.heading = float(json_num(m, "h", g_drive.heading));
			g_drive.pitch = float(json_num(m, "p", g_drive.pitch));
			g_drive.lookUntil = natives::GetGameTimer() + 3000;
		}
		else if (op == "blockprops")
		{
			props_clear_all();
			const std::string model = json_str(m, "model");
			if (!model.empty())
				g_props.model = model;
			g_props.visible = json_num(m, "visible", 1.0) != 0.0;
			g_props.alpha = int(json_num(m, "alpha", 255));
			g_props.hash = 0;
			g_props.mn = g_props.mx = {};
			g_ws.send("{\"t\":\"blocksync\",\"r\":64}");
		}
		else if (op == "dims")
		{
			const std::string model = json_str(m, "model");
			const Hash h = natives::GetHashKey(model.c_str());
			Vector3 mn = {}, mx = {};
			const BOOL valid = natives::IsModelValid(h);
			if (valid)
			{
				natives::RequestModel(h);
				for (int i = 0; i < 100 && !natives::HasModelLoaded(h); ++i)
					WAIT(0);
				natives::GetModelDimensions(h, &mn, &mx);
			}
			sendf("{\"t\":\"gtainfo\",\"model\":\"%s\",\"valid\":%d,\"min\":[%.3f,%.3f,%.3f],\"max\":[%.3f,%.3f,%.3f]}",
				model.c_str(), valid ? 1 : 0, mn.x, mn.y, mn.z, mx.x, mx.y, mx.z);
		}
		send_state(ped);
	}

	bool props_model_ready()
	{
		if (g_props.hash == 0)
			g_props.hash = natives::GetHashKey(g_props.model.c_str());
		if (!natives::IsModelValid(g_props.hash))
			return false;
		natives::RequestModel(g_props.hash);
		if (!natives::HasModelLoaded(g_props.hash))
			return false;
		if (g_props.mx.x == g_props.mn.x)
			natives::GetModelDimensions(g_props.hash, &g_props.mn, &g_props.mx);
		return true;
	}

	void props_clear_all()
	{
		for (auto &[key, obj] : g_props.live)
			if (natives::DoesEntityExist(obj))
				natives::DeleteObject(&obj);
		g_props.live.clear();
		g_props.handles.clear();
		g_props.turns.clear();
		g_props.pending.clear();
		g_props.blocks.clear();
	}

	void prop_remove(const std::tuple<int, int, int> &key);

	/// A mined pit's floor or side cell solid for GTA (`on`), or not any more.

	/// The way a block's prop turns: its thin side (0.80 m) across the wall the block is part of, so a wall of
	/// blocks is closed (no slits for GTA's bullets or eyes). 0: the wall runs along x, 1: along z.
	int prop_turn(const std::tuple<int, int, int> &key)
	{
		const int x = std::get<0>(key), y = std::get<1>(key), z = std::get<2>(key);
		auto has = [&](int dx, int dz) { return g_props.blocks.count(std::make_tuple(x + dx, y, z + dz)) != 0; };
		return has(1, 0) || has(-1, 0) ? 0 : has(0, 1) || has(0, -1) ? 1 : 0;
	}

	/// Put a fresh prop on its block: on its side (the 0.96-0.97 m sides up and along the wall, see prop_turn) and
	/// centred on the block. If GTA won't turn it that way, it stands upright on the block's floor as it used to.
	void prop_place(Object o, const std::tuple<int, int, int> &key, int turn)
	{
		const auto [bx, by, bz] = key;
		const float gx = bx + 0.5f, gy = -(bz + 0.5f), gz = by - g_yOffset + 0.5f;
		const Vector3 &mn = g_props.mn, &mx = g_props.mx;
		bool turned = false;
		float yaw = turn == 0 ? 0.0f : 90.0f;
		for (int attempt = 0; attempt < 2 && !turned; ++attempt, yaw += 90.0f)
		{
			natives::SetEntityRotation(o, 90.0f, 0.0f, yaw);
			const Vector3 at = natives::GetEntityCoords(o, TRUE);
			const Vector3 thin = natives::GetOffsetFromEntityInWorldCoords(o, 0.0f, 0.0f, 1.0f); // the model's z: its thin side
			const float tx = std::fabs(thin.x - at.x), ty = std::fabs(thin.y - at.y), tz = std::fabs(thin.z - at.z);
			if (tz > 0.5f)
				break; // the turn didn't take
			turned = (turn == 0) == (ty > tx); // across GTA y (Minecraft z) for a wall along x, else across x
		}
		if (!turned)
			natives::SetEntityRotation(o, 0, 0, 0);
		// wherever the turn put the box's centre, move it onto the block's (upright: sitting on the floor)
		const Vector3 at = natives::GetEntityCoords(o, TRUE);
		const Vector3 c = natives::GetOffsetFromEntityInWorldCoords(o, (mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f);
		const float wantZ = turned ? gz : gz - 0.5f + (mx.z - mn.z) * 0.5f + 0.01f;
		natives::SetEntityCoordsNoOffset(o, at.x + gx - c.x, at.y + gy - c.y, at.z + wantZ - c.z);
	}

	void prop_remove(const std::tuple<int, int, int> &key)
	{
		auto it = g_props.live.find(key);
		if (it == g_props.live.end())
			return;
		g_props.handles.erase(it->second);
		if (natives::DoesEntityExist(it->second))
			natives::DeleteObject(&it->second);
		g_props.live.erase(it);
		g_props.turns.erase(key);
	}

	/// Minecraft block (x, y, z)'s centre's squared distance from GTA point `me`.
	float block_dist2(const std::tuple<int, int, int> &key, const Vector3 &me)
	{
		const float dx = std::get<0>(key) + 0.5f - me.x, dy = -(std::get<2>(key) + 0.5f) - me.y, dz = std::get<1>(key) + 0.5f - g_yOffset - me.z;
		return dx * dx + dy * dy + dz * dz;
	}

	/// Twice a second: props for the Minecraft blocks within kRadius of the player, nearest first (up to kMax); props
	/// of blocks left behind (a little past the radius) go, so the nearest always have one.
	void props_pick(const Vector3 &me)
	{
		const int now = natives::GetGameTimer();
		if (now < g_props.nextPickAt)
			return;
		g_props.nextPickAt = now + 500;
		const float r2 = BlockProps::kRadius * BlockProps::kRadius, keep2 = (BlockProps::kRadius + 4.0f) * (BlockProps::kRadius + 4.0f);
		std::vector<std::pair<float, std::tuple<int, int, int>>> want;
		for (const auto &key : g_props.blocks)
		{
			const float d2 = block_dist2(key, me);
			if (d2 >= r2)
				continue;
			// (one buried on all six sides can't be touched: its prop is better spent further off)
			const auto [x, y, z] = key;
			if (g_props.blocks.count({x + 1, y, z}) && g_props.blocks.count({x - 1, y, z}) && g_props.blocks.count({x, y + 1, z}) &&
				g_props.blocks.count({x, y - 1, z}) && g_props.blocks.count({x, y, z + 1}) && g_props.blocks.count({x, y, z - 1}))
				continue;
			want.emplace_back(d2, key);
		}
		if (want.size() > BlockProps::kMax)
		{
			std::nth_element(want.begin(), want.begin() + BlockProps::kMax, want.end(), [](auto &a, auto &b) { return a.first < b.first; });
			want.resize(BlockProps::kMax);
		}
		std::set<std::tuple<int, int, int>> wanted;
		for (const auto &[d2, key] : want)
			wanted.insert(key);
		std::vector<std::tuple<int, int, int>> drop;
		for (const auto &[key, obj] : g_props.live)
			if (!wanted.count(key) && (block_dist2(key, me) > keep2 || g_props.live.size() - drop.size() > BlockProps::kMax - 20))
				drop.push_back(key);
		for (const auto &key : drop)
			prop_remove(key);
		// the queue: wanted blocks without a prop, nearest last (taken first)
		std::sort(want.begin(), want.end(), [](auto &a, auto &b) { return a.first > b.first; });
		g_props.pending.clear();
		for (const auto &[d2, key] : want)
			if (!g_props.live.count(key))
				g_props.pending.push_back(key);
	}

	/// A block came or went: its neighbours along a wall may want turning the other way now.
	void props_neighbours_changed(const std::tuple<int, int, int> &key)
	{
		const auto [x, y, z] = key;
		for (const auto &[dx, dz] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}})
		{
			const auto n = std::make_tuple(x + dx, y, z + dz);
			const auto t = g_props.turns.find(n);
			if (t != g_props.turns.end() && t->second != prop_turn(n))
			{
				prop_remove(n);
				g_props.pending.push_back(n);
			}
		}
	}

	/// Whether a car or a person (the player too) is in block `key`'s space.
	std::unordered_set<Entity> g_keptCars; // cars kept from GTA's clean-up: they're by Minecraft's blocks

	bool block_occupied(const std::tuple<int, int, int> &key)
	{
		const auto [bx, by, bz] = key;
		const float cx = bx + 0.5f, cy = -(bz + 0.5f), cz = by - g_yOffset + 0.5f;
		bool busy = false;
		each_vehicle_near(cx, cy, cz, 8.0f, [&](Vehicle v, float, float, float) {
			if (busy)
				return;
			Vector3 mn = {}, mx = {};
			natives::GetModelDimensions(natives::GetEntityModel(v), &mn, &mx);
			const Vector3 l = natives::GetOffsetFromEntityGivenWorldCoords(v, cx, cy, cz);
			busy = l.x > mn.x - 0.55f && l.x < mx.x + 0.55f && l.y > mn.y - 0.55f && l.y < mx.y + 0.55f && l.z > mn.z - 0.55f && l.z < mx.z + 0.55f;
		});
		if (busy)
			return true;
		int handles[256];
		const int n = worldGetAllPeds(handles, 256);
		for (int i = 0; i < n && !busy; ++i)
		{
			const Vector3 o = natives::GetEntityCoords(handles[i], TRUE);
			busy = std::fabs(o.x - cx) < 0.85f && std::fabs(o.y - cy) < 0.85f && o.z - 0.95f < cz + 0.5f && o.z + 0.8f > cz - 0.5f;
		}
		return busy;
	}

	/// Props waiting for their space to clear get their collision once it has.
	void props_ghosts_tick()
	{
		const int now = natives::GetGameTimer();
		static int nextKeptCheck = 0;
		if (!g_keptCars.empty() && now >= nextKeptCheck)
		{
			nextKeptCheck = now + 1000;
			const Vector3 me = natives::GetEntityCoords(natives::PlayerPedId(), TRUE);
			for (auto it = g_keptCars.begin(); it != g_keptCars.end();)
			{
				Entity v = *it;
				if (!natives::DoesEntityExist(v))
				{
					it = g_keptCars.erase(it);
					continue;
				}
				const Vector3 o = natives::GetEntityCoords(v, TRUE);
				if ((o.x - me.x) * (o.x - me.x) + (o.y - me.y) * (o.y - me.y) > 250.0f * 250.0f)
				{
					natives::SetEntityAsNoLongerNeeded(&v);
					it = g_keptCars.erase(it);
				}
				else
					++it;
			}
		}
		if (g_props.ghosts.empty() || now < g_props.nextGhostAt)
			return;
		g_props.nextGhostAt = now + 250;
		for (auto it = g_props.ghosts.begin(); it != g_props.ghosts.end();)
		{
			const auto live = g_props.live.find(*it);
			if (live == g_props.live.end())
				it = g_props.ghosts.erase(it);
			else if (!block_occupied(*it))
			{
				natives::SetEntityCollision(live->second, TRUE, TRUE);
				it = g_props.ghosts.erase(it);
			}
			else
				++it;
		}
	}

	/// Cars that crash into Minecraft's blocks (their props) dent three times as deep as GTA's own crash: GTA's own
	/// dent, and twice that again where the car hit, by how much speed the crash took off it.
	void crash_tick(Ped player)
	{
		if (g_props.live.empty())
		{
			g_carVelocity.clear();
			return;
		}
		const Vector3 me = natives::GetEntityCoords(player, TRUE);
		std::unordered_map<Vehicle, Vector3> next;
		each_vehicle_near(me.x, me.y, me.z, 60.0f, [&](Vehicle v, float, float, float) {
			const Vector3 vel = natives::GetEntityVelocity(v);
			next[v] = vel;
			const auto it = g_carVelocity.find(v);
			if (it == g_carVelocity.end())
				return;
			const Vector3 pv = it->second;
			const float before = std::sqrt(pv.x * pv.x + pv.y * pv.y + pv.z * pv.z), after = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
			if (before - after < 4.0f || before < 0.1f)
				return;
			// the block it ran into: the nearest one ahead of where it was going, touching the car (not a block a few
			// metres off when it hit something of GTA's)
			const Vector3 c = natives::GetEntityCoords(v, TRUE);
			Vector3 mn = {}, mx = {};
			natives::GetModelDimensions(natives::GetEntityModel(v), &mn, &mx);
			float best = 5.0f * 5.0f;
			Vector3 at = {};
			bool found = false;
			for (const auto &[key, obj] : g_props.live)
			{
				const auto [bx, by, bz] = key;
				const float x = bx + 0.5f - c.x, y = -(bz + 0.5f) - c.y, z = by - g_yOffset + 0.5f - c.z, d2 = x * x + y * y + z * z;
				if (d2 >= best || x * pv.x + y * pv.y + z * pv.z <= 0.0f)
					continue;
				const Vector3 local = natives::GetOffsetFromEntityGivenWorldCoords(v, bx + 0.5f, -(bz + 0.5f), by - g_yOffset + 0.5f);
				const float ox = std::max({mn.x - local.x, local.x - mx.x, 0.0f}), oy = std::max({mn.y - local.y, local.y - mx.y, 0.0f}),
							oz = std::max({mn.z - local.z, local.z - mx.z, 0.0f});
				if (ox * ox + oy * oy + oz * oz > 0.9f * 0.9f) // (the block's middle half a block and a bit from the body)
					continue;
				{
					best = d2;
					const float d = std::sqrt(d2);
					at.x = c.x + x - x / d * 0.5f; // the block's face toward the car
					at.y = c.y + y - y / d * 0.5f;
					at.z = c.z + z - z / d * 0.5f;
					found = true;
				}
			}
			if (found)
				dent_vehicle(v, at, (before - after) * 80.0f, 1.2f);
		});
		g_carVelocity = std::move(next);
	}

	/// Spawn a few queued props per frame (the model has to be streamed in first).
	void props_tick(const Vector3 &me)
	{
		props_pick(me);
		props_ghosts_tick();
		if (g_props.pending.empty() || !props_model_ready())
			return;
		// (Minecraft block (x,y,z) spans GTA x..x+1, -z-1..-z, y-yOffset..+1)
		int n = 0;
		while (!g_props.pending.empty() && n < 30 && g_props.live.size() < BlockProps::kMax)
		{
			const auto key = g_props.pending.back();
			g_props.pending.pop_back();
			if (g_props.live.count(key) || !g_props.blocks.count(key) || block_dist2(key, me) > BlockProps::kRadius * BlockProps::kRadius)
				continue; // already there, gone again before its turn, or too far away
			const auto [bx, by, bz] = key;
			const Object o = natives::CreateObjectNoOffset(g_props.hash, bx + 0.5f, -(bz + 0.5f), by - g_yOffset + 0.5f);
			if (o == 0)
				break; // no room: try again later
			natives::FreezeEntityPosition(o, TRUE);
			// cars by Minecraft's blocks stay (GTA's population took them away, built round or into)
			each_vehicle_near(bx + 0.5f, -(bz + 0.5f), by - g_yOffset + 0.5f, 8.0f, [&](Vehicle v, float, float, float) {
				// (a mission's own car is the mission's: left alone; traffic driving by isn't built round)
				if (g_keptCars.count(v) || natives::IsEntityAMissionEntity(v))
					return;
				const Vector3 vel = natives::GetEntityVelocity(v);
				if (vel.x * vel.x + vel.y * vel.y + vel.z * vel.z > 1.0f)
					return;
				Vector3 mn = {}, mx = {};
				natives::GetModelDimensions(natives::GetEntityModel(v), &mn, &mx);
				const Vector3 local = natives::GetOffsetFromEntityGivenWorldCoords(v, bx + 0.5f, -(bz + 0.5f), by - g_yOffset + 0.5f);
				const float ox = std::max({mn.x - local.x, local.x - mx.x, 0.0f}), oy = std::max({mn.y - local.y, local.y - mx.y, 0.0f}),
							oz = std::max({mn.z - local.z, local.z - mx.z, 0.0f});
				if (ox * ox + oy * oy + oz * oz <= 1.2f * 1.2f) // (touching the block, or in its space)
				{
					natives::SetEntityAsMissionEntity(v);
					g_keptCars.insert(v);
				}
			});
			const int turn = prop_turn(key);
			prop_place(o, key, turn);
			g_props.turns[key] = turn;
			natives::SetEntityCanBeDamaged(o, FALSE);
			natives::SetDisableFragDamage(o, TRUE);
			if (block_occupied(key))
			{
				natives::SetEntityCollision(o, FALSE, FALSE); // until its space is clear (props_ghosts_tick)
				g_props.ghosts.insert(key);
			}
			else
				natives::SetEntityCollision(o, TRUE, TRUE);
			natives::SetEntityLodDist(o, 200);
			if (!g_props.visible)
				natives::SetEntityVisible(o, FALSE, FALSE);
			else if (g_props.alpha < 255)
				natives::SetEntityAlpha(o, g_props.alpha);
			g_props.live[key] = o;
			g_props.handles.insert(o);
			++n;
		}
	}

	void props_message(const std::string &m)
	{
		auto parse = [&](const char *field, auto &&each) {
			const char *p = json_value(m, field);
			if (p == nullptr || *p != '[')
				return;
			++p;
			int v[3], k = 0;
			while (*p && *p != ']')
			{
				char *end = nullptr;
				const long x = std::strtol(p, &end, 10);
				if (end == p)
				{
					++p;
					continue;
				}
				v[k++] = int(x);
				p = end;
				if (k == 3)
				{
					each(std::make_tuple(v[0], v[1], v[2]));
					k = 0;
				}
			}
		};
		parse("clear", [&](auto key) {
			g_props.blocks.erase(key);
			prop_remove(key);
			props_neighbours_changed(key);
		});
		parse("set", [&](auto key) {
			g_props.blocks.insert(key);
			g_props.pending.push_back(key);
			props_neighbours_changed(key);
		});
	}

	/// Messages from Minecraft: explosions become GTA explosions at the same spot; director commands.
	void handle_event(const std::string &message)
	{
		const std::string type = json_str(message, "t");
		snprintf(g_doingMessage, sizeof(g_doingMessage), "Minecraft's \"%s\"", type.c_str());
		g_doing = g_doingMessage;
		if (type == "gta")
		{
			// (the video tools' scripted shots, tools/video: only with Director=1, never in play)
			if (g_settings.director)
				handle_director(message);
			return;
		}
		if (type == "blocks")
		{
			props_message(message);
			return;
		}
		if (type == "mcpos")
		{
			// while Minecraft moves the player: only once it does, and has the last place GTA put it at
			const bool mcMoves = g_walk.on || g_carFly.on;
			if (mcMoves && (json_num(message, "w", 0.0) == 0.0 || int(json_num(message, "ps", -1.0)) != g_walk.psetId))
				return;
			if (mcMoves)
			{
				g_walk.acked = true;
				g_walk.ground = json_num(message, "g", 1.0) != 0.0;
				if (g_walk.ground)
					g_walk.groundAt = natives::GetGameTimer();
				const char *fly = json_value(message, "fly");
				g_mcFlying = fly != nullptr && std::strncmp(fly, "true", 4) == 0;
			}
			const char *pos = json_value(message, "pos");
			const char *vel = json_value(message, "vel");
			double x, y, z, vx = 0, vy = 0, vz = 0;
			if (pos && sscanf_s(pos, "[%lf ,%lf ,%lf ]", &x, &y, &z) == 3)
			{
				if (vel)
					sscanf_s(vel, "[%lf ,%lf ,%lf ]", &vx, &vy, &vz);
				g_drive.pos = {float(x), 0, float(-z), 0, float(y - g_yOffset), 0};
				g_drive.vel = {float(vx), 0, float(-vz), 0, float(vy), 0};
				const double tn = json_num(message, "tn", 0.0);
				g_drive.posNanos = tn > 0.0 ? int64_t(tn) : now_nanos();
				g_drive.havePos = true;
			}
			return;
		}
		if (type == "proj")
		{
			projectiles_message(natives::PlayerPedId(), message);
			return;
		}
		if (type == "melee")
		{
			Enchants en;
			en.knockback = int(json_num(message, "kb", 0.0));
			en.sharpness = int(json_num(message, "sh", 0.0));
			en.fire = int(json_num(message, "fa", 0.0));
			en.looting = int(json_num(message, "lo", 0.0));
			if (!g_noInput)
				melee(natives::PlayerPedId(), json_str(message, "k"), float(json_num(message, "s", 1.0)), en);
			return;
		}
		if (type == "mobs")
		{
			mobs_message(message);
			return;
		}
		if (type == "hot")
		{
			hot_message(message);
			return;
		}
		if (type == "water")
		{
			water_message(message);
			return;
		}
		if (type == "inportal")
		{
			const char *on = json_value(message, "on");
			g_fx.warpTarget = on != nullptr && std::strncmp(on, "true", 4) == 0 ? 0.85f : 0.0f;
			return;
		}
		if (type == "nether")
		{
			if (json_value(message, "on") != nullptr && std::strncmp(json_value(message, "on"), "true", 4) == 0)
				hell_start(message);
			else
				hell_stop();
			return;
		}
		if (type == "end")
		{
			if (json_value(message, "on") != nullptr && std::strncmp(json_value(message, "on"), "true", 4) == 0)
				end_start(message);
			else
				end_stop();
			return;
		}
		if (type == "screen")
		{
			// Minecraft opened or closed a screen (chat, inventory, ...): {"t":"screen","kind":0|1|2|3}
			g_screen = int(json_num(message, "kind", 0));
			return;
		}
		if (type == "note")
		{
			const std::string text = json_str(message, "text");
			if (!text.empty())
				natives::Notify(text.c_str());
			return;
		}
		if (type == "mobhit")
		{
			if (!g_noInput)
				mobhit_message(natives::PlayerPedId(), message);
			return;
		}
		if (type == "leashuse")
		{
			leash_use(natives::PlayerPedId(), message);
			return;
		}
		if (type == "leashgone")
		{
			leash_gone(int(json_num(message, "id", 0.0)));
			return;
		}
		if (type == "boats")
		{
			boats_message(message);
			return;
		}
		if (type == "riptide")
		{
			// a trident's riptide launched Minecraft's player: Minecraft moves it for the flight, GTA's player follows
			g_walk.forceUntil = natives::GetGameTimer() + 3500;
			logf("riptide: Minecraft's flight");
			return;
		}
		if (type == "bowdraw")
		{
			g_bowDrawn = message.find("\"on\":true") != std::string::npos;
			g_bowAt = natives::GetGameTimer();
			return;
		}
		if (type == "enderthief")
		{
			enderthief_message(natives::PlayerPedId(), message);
			return;
		}
		if (type == "allmobs")
		{
			world_mobs_message(message);
			return;
		}
		if (type == "animal")
		{
			animal_message(message);
			return;
		}
		if (type == "gtacmd")
		{
			command_message(natives::PlayerPedId(), message);
			return;
		}
		if (type == "pdmg")
		{
			// Minecraft's player was hurt (survival): GTA's player takes it, in hearts of its own health
			const Ped me = natives::PlayerPedId();
			const int max = natives::GetEntityMaxHealth(me) - 100;
			if (!g_godMode && max > 0 && !g_noInput)
				natives::ApplyDamageToPed(me, std::max(1, int(json_num(message, "d", 0.0) * max / 20.0)));
			return;
		}
		if (type == "ate")
		{
			// eaten in Minecraft: stamina back (a steak fills it), and some health
			const int n = int(json_num(message, "n", 0.0));
			g_stamina = std::min(100.0f, g_stamina + 14.0f * float(n));
			if (g_stamina >= 33.0f)
				g_exhausted = false;
			const Ped me = natives::PlayerPedId();
			const int max = natives::GetEntityMaxHealth(me) - 100;
			if (max > 0 && !natives::IsEntityDead(me))
				natives::SetEntityHealth(me, std::min(natives::GetEntityMaxHealth(me), natives::GetEntityHealth(me) + max * n / 40));
			return;
		}
		if (type == "pheal")
		{
			// Minecraft's player healed (a potion, regeneration, food): GTA's player too
			const Ped me = natives::PlayerPedId();
			const int max = real_max_health(me) - 100;
			if (max > 0 && !g_noInput && !natives::IsEntityDead(me))
				natives::SetEntityHealth(me, std::min(natives::GetEntityMaxHealth(me), std::min(real_max_health(me), real_health(me) +
					std::max(1, int(json_num(message, "d", 0.0) * max / 20.0))) + (me == g_totemPed ? g_totemBuffer : 0)));
			return;
		}
		if (type == "effects")
		{
			// Minecraft's effects on its player, as GTA's player feels them
			g_mcEffects = json_str(message, "e");
			return;
		}
		if (type == "pstate")
		{
			// what Minecraft's player wears (the elytra), and whether it's a spectator
			g_mcElytra = json_num(message, "ely", 0.0) != 0.0;
			g_mcBlocking = json_num(message, "blk", 0.0) != 0.0;
			g_mcTotem = json_num(message, "tot", 0.0) != 0.0;
			g_mcScoping = json_num(message, "scope", 0.0) != 0.0;
			// Minecraft's armour worn protects GTA's player too, as in Minecraft: 4% less damage a point (80% at 20)
			{
				const int armor = int(json_num(message, "arm", 0.0));
				static int applied = -1;
				if (armor != applied)
				{
					applied = armor;
					const Player me = natives::PlayerId();
					const float k = std::max(0.2f, 1.0f - 0.04f * float(std::clamp(armor, 0, 20)));
					natives::SetPlayerWeaponDefenseModifier(me, k);
					natives::SetPlayerMeleeWeaponDefenseModifier(me, k);
					natives::SetPlayerVehicleDefenseModifier(me, k);
				}
			}
			g_mcSpectator = json_num(message, "spec", 0.0) != 0.0;
			return;
		}
		if (type == "pteleport")
		{
			// Minecraft moved the player (ender pearl): move GTA's player there, keeping the height mapping (while
			// Minecraft moves the player GTA's follows by itself: the jump in position is expected)
			if (g_walk.on)
			{
				g_walk.teleportAt = natives::GetGameTimer();
				return;
			}
			const char *pos = json_value(message, "pos");
			double x, y, z;
			if (pos && sscanf_s(pos, "[%lf ,%lf ,%lf ]", &x, &y, &z) == 3)
			{
				const Ped me = natives::PlayerPedId();
				if (natives::IsPedInAnyVehicle(me, FALSE))
				{
					// thrown from a car: the car goes there, with the player in it
					const Vehicle car = natives::GetVehiclePedIsIn(me, FALSE);
					natives::SetEntityCoordsNoOffset(car, float(x), float(-z), float(y - g_yOffset + 1.0));
					g_carFly.teleportAt = natives::GetGameTimer();
				}
				else
					natives::SetEntityCoordsNoOffset(me, float(x), float(-z), float(y - g_yOffset + 1.0));
			}
			return;
		}
		if (type != "explosion" || g_noInput)
			return; // (no explosions in GTA's cutscenes and mission scenes: they could kill someone the mission needs)
		const char *pos = json_value(message, "pos");
		double x, y, z;
		if (pos == nullptr || sscanf_s(pos, "[%lf ,%lf ,%lf ]", &x, &y, &z) != 3)
			return;
		const double radius = json_num(message, "r", 4.0);
		g_booms[g_boomNext++ % 8] = {float(x), float(-z), float(y - g_yOffset), natives::GetGameTimer() + 500};
		const std::string src = json_str(message, "src");
		if (src == "fireball" || src == "wither_skull" || src == "dragon_fireball")
		{
			// a ghast's fireball: a rocket's blast and burning fuel
			natives::AddExplosion(float(x), float(-z), float(y - g_yOffset), 4, 1.0f, TRUE, FALSE, 0.0f, FALSE);
			natives::AddExplosion(float(x), float(-z), float(y - g_yOffset), 3, 1.0f, TRUE, FALSE, 0.0f, FALSE);
			shake_from(float(x), float(-z), float(y - g_yOffset), 0.8f);
			return;
		}
		// TNT (radius 4) as a sticky bomb, smaller blasts (creepers are 3) as grenades.
		natives::AddExplosion(float(x), float(-z), float(y - g_yOffset), radius >= 3.5 ? 2 : 0, 1.0f, TRUE, FALSE, 0.0f, FALSE);
		shake_from(float(x), float(-z), float(y - g_yOffset), radius >= 3.5 ? 1.1f : 0.8f);
	}

	/// Minecraft-driven flight: follow Minecraft's player with a smoothed chase camera; GTA's (invisible) player rides
	/// along so the world streams in around it. Steering = where Steve looks: the director's flylook, or the mouse.
	void drive_tick(Ped ped)
	{
		const int64_t now = now_nanos();
		const float frameDt = std::clamp(float(now - g_drive.lastTick) * 1e-9f, 0.0f, 0.1f);
		g_drive.lastTick = now;
		if (natives::GetGameTimer() > g_drive.lookUntil)
		{
			if (g_drive.user)
			{
				// the mouse steers, read directly (GTA's own camera isn't the one rendering)
				g_drive.heading -= natives::GetDisabledControlNormal(0, 1) * 6.0f;
				g_drive.pitch = std::clamp(g_drive.pitch - natives::GetDisabledControlNormal(0, 2) * 6.0f, -80.0f, 70.0f);
			}
			else
			{
				const Vector3 r = natives::GetGameplayCamRot(2);
				g_drive.heading = r.z;
				g_drive.pitch = std::clamp(r.x, -70.0f, 70.0f);
			}
		}
		const float look = 1.0f - std::exp(-frameDt * (g_drive.user ? 16.0f : 8.0f));
		g_drive.sHeading += (std::fmod(g_drive.heading - g_drive.sHeading + 540.0f, 360.0f) - 180.0f) * look;
		g_drive.sPitch += (g_drive.pitch - g_drive.sPitch) * look;
		if (!g_drive.havePos)
			return;
		// where Minecraft's player is now: its last sample, carried forward by its velocity to this moment
		const float dt = std::clamp(float(now - g_drive.posNanos) * 1e-9f, 0.0f, 0.1f);
		const float px = g_drive.pos.x + g_drive.vel.x * dt, py = g_drive.pos.y + g_drive.vel.y * dt, pz = g_drive.pos.z + g_drive.vel.z * dt;
		const float ex = px - g_drive.estX, ey = py - g_drive.estY, ez = pz - g_drive.estZ;
		if (!g_drive.haveEst || ex * ex + ey * ey + ez * ez > 8.0f * 8.0f)
		{
			g_drive.estX = px; g_drive.estY = py; g_drive.estZ = pz;
			g_drive.haveEst = true;
		}
		else
		{
			const float ease = 1.0f - std::exp(-frameDt * 12.0f);
			g_drive.estX += g_drive.vel.x * frameDt + (px - g_drive.estX - g_drive.vel.x * frameDt) * ease;
			g_drive.estY += g_drive.vel.y * frameDt + (py - g_drive.estY - g_drive.vel.y * frameDt) * ease;
			g_drive.estZ += g_drive.vel.z * frameDt + (pz - g_drive.estZ - g_drive.vel.z * frameDt) * ease;
		}
		const float tx = g_drive.estX, ty = g_drive.estY, tz = g_drive.estZ;
		if (g_drive.user && g_drive.vel.z < 0.0f && natives::GetGameTimer() - g_drive.launchedAt > 1000)
		{
			// Minecraft's player goes through GTA's world (it isn't in Minecraft): land on whatever is below instead
			float g = 0.0f;
			if (natives::GetGroundZFor3dCoord(tx, ty, tz + 1.0f, &g, FALSE, FALSE) && tz + g_drive.vel.z * 0.05f < g + 0.4f)
			{
				drive_land(ped, tx, ty, g);
				return;
			}
		}
		natives::SetEntityCoordsNoOffset(ped, tx, ty, tz + 1.0f);
		const float d2r = 3.14159265f / 180.0f;
		const float h = g_drive.sHeading * d2r, pt = g_drive.sPitch * d2r;
		const float fx = -std::sin(h) * std::cos(pt), fy = std::cos(h) * std::cos(pt), fz = std::sin(pt);
		const float cx = tx - fx * g_drive.dist, cy = ty - fy * g_drive.dist, cz = tz + 1.2f + g_drive.height - fz * g_drive.dist;
		if (!g_drive.camInit)
		{
			g_drive.camX = cx; g_drive.camY = cy; g_drive.camZ = cz;
			g_drive.camInit = true;
		}
		// frame-rate independent smoothing (21/s is 0.3 a frame at 60 fps), ramping up after the switch from GTA's camera
		g_drive.follow = std::min(21.0f, g_drive.follow + frameDt * 30.0f);
		const float k = 1.0f - std::exp(-frameDt * g_drive.follow);
		g_drive.camX += (cx - g_drive.camX) * k;
		g_drive.camY += (cy - g_drive.camY) * k;
		g_drive.camZ += (cz - g_drive.camZ) * k;
		// look at a point ahead of Steve so he sits a little below the centre of the frame
		const float ax = tx + fx * 6.0f - g_drive.camX, ay = ty + fy * 6.0f - g_drive.camY, az = tz + 1.0f + fz * 6.0f - g_drive.camZ;
		const float camHeading = std::atan2(-ax, ay) / d2r, camPitch = std::atan2(az, std::sqrt(ax * ax + ay * ay)) / d2r;
		natives::SetCamCoord(g_drive.cam, g_drive.camX, g_drive.camY, g_drive.camZ);
		natives::SetCamRot(g_drive.cam, camPitch, 0.0f, camHeading);
		natives::SetCamFov(g_drive.cam, g_drive.fov);
		natives::SetCamMotionBlurStrength(g_drive.cam, 0.0f);
		natives::SetCamUseShallowDofMode(g_drive.cam, FALSE);
		natives::SetCamDofStrength(g_drive.cam, 0.0f);
		g_drive.outX = g_drive.camX; g_drive.outY = g_drive.camY; g_drive.outZ = g_drive.camZ;
		g_drive.outPitch = camPitch; g_drive.outHeading = camHeading; g_drive.outFov = g_drive.fov;
		g_drive.steveX = tx; g_drive.steveY = ty; g_drive.steveZ = tz;
		g_drive.haveOut = true;
	}

	/// GTA's ground is at fractional heights but blocks sit on whole ones: when nothing Minecraft is built nearby,
	/// re-level the mapping so the ground here is whole again (built things keep their place).
	void maybe_relevel(const Vector3 &player)
	{
		if (mobs_active())
			return; // a relevel clears all of Minecraft's ground: the mobs would fall through the world
		static int next = 0, lastAt = -100000;
		const int now = natives::GetGameTimer();
		if (now < next || now - lastAt < 30000)
			return; // (at most every half a minute)
		next = now + 1500;
		// not again until well away from where it last levelled: on slopes, stairs and uneven streets the ground is never
		// on a whole block for long, and each relevel rebuilt all of Minecraft's ground around (a hitch) and, in Minecraft's
		// movement, put its player back where it was (stopped dead, every 1.5 s)
		const float lx = player.x - g_levelX, ly = player.y - g_levelY;
		if (lx * lx + ly * ly < 40.0f * 40.0f)
			return;
		// Minecraft moving the player: only while it stands still on the ground (where putting it there again is nothing)
		if (g_walk.on && (!g_walk.ground || g_drive.vel.x * g_drive.vel.x + g_drive.vel.y * g_drive.vel.y > 0.04f))
			return;
		float groundZ = 0.0f;
		if (!natives::GetGroundZFor3dCoord(player.x, player.y, player.z + 1.0f, &groundZ, FALSE, FALSE))
			return;
		const float mc = groundZ + g_yOffset;
		if (std::fabs(mc - std::round(mc)) < 0.3f)
			return;
		const int px = int(std::floor(player.x)), pz = int(std::floor(-player.y));
		for (const auto &[key, obj] : g_props.live)
		{
			const int dx = std::get<0>(key) - px, dz = std::get<2>(key) - pz;
			if (dx * dx + dz * dz < 24 * 24)
				return; // blocks nearby: keep them where they are
		}
		logf("relevel: the ground here is %.2f off a whole block", mc - std::round(mc));
		lastAt = now;
		g_haveOffset = false; // the tick re-levels to the ground here
	}

	/// GTA control `control` as Minecraft's key `key`: its down and up, by whether it's held now against what Minecraft
	/// was last told (not GTA's just-pressed and just-released: a frame not forwarded lost the release, and the bow was
	/// never let go). `vk`: the mouse button itself, read while the plugin holds the control down for GTA (a bow drawn in
	/// a mission aims GTA's gun: GTA's own control never showed the release then).
	void forward_button(int control, const char *key, int vk = 0)
	{
		static std::unordered_map<int, bool> told;
		bool down = natives::IsDisabledControlPressed(0, control) != FALSE;
		if (vk != 0 && natives::IsUsingKeyboardAndMouse())
			down = (GetAsyncKeyState(vk) & 0x8000) != 0;
		bool &was = told[control];
		if (down != was)
		{
			was = down;
			sendf("{\"t\":\"key\",\"k\":\"%s\",\"down\":%s}", key, down ? "true" : "false");
		}
	}

	/// Shift, Ctrl and Alt as Minecraft's modifier bits (SDL's: shift 1, ctrl 64, alt 256).
	int key_mods()
	{
		return ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 1 : 0) | ((GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 64 : 0) |
			((GetAsyncKeyState(VK_MENU) & 0x8000) ? 256 : 0);
	}

	/// A Windows keyboard scan code (set 1, with its extended flag) as Minecraft's key code (SDL's scancode, the
	/// key's place on a US keyboard, as Minecraft's own key bindings use), or 0.
	int hid_scancode(BYTE scan, BOOL extended)
	{
		static const unsigned char kBase[0x59] = {
			0, 41, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 45, 46, 42, 43,   // 00: Esc 1 2 3 4 5 6 7 8 9 0 - = Backspace Tab
			20, 26, 8, 21, 23, 28, 24, 12, 18, 19, 47, 48, 40, 224, 4, 22,   // 10: Q W E R T Y U I O P [ ] Enter LCtrl A S
			7, 9, 10, 11, 13, 14, 15, 51, 52, 53, 225, 49, 29, 27, 6, 25,    // 20: D F G H J K L ; ' ` LShift \ Z X C V
			5, 17, 16, 54, 55, 56, 229, 85, 226, 44, 57, 58, 59, 60, 61, 62, // 30: B N M , . / RShift KP* LAlt Space Caps F1-F5
			63, 64, 65, 66, 67, 83, 71, 95, 96, 97, 86, 92, 93, 94, 87, 89,  // 40: F6-F10 NumLock ScrollLock KP7 KP8 KP9 KP- KP4 KP5 KP6 KP+ KP1
			90, 91, 98, 99, 0, 0, 100, 68, 69,                               // 50: KP2 KP3 KP0 KP. - - <> F11 F12
		};
		if (!extended)
			return scan < sizeof(kBase) ? kBase[scan] : 0;
		switch (scan)
		{
		case 0x1C: return 88;  // keypad Enter
		case 0x1D: return 228; // right Ctrl
		case 0x35: return 84;  // keypad /
		case 0x38: return 230; // right Alt (AltGr)
		case 0x47: return 74;  // Home
		case 0x48: return 82;  // Up
		case 0x49: return 75;  // Page Up
		case 0x4B: return 80;  // Left
		case 0x4D: return 79;  // Right
		case 0x4F: return 77;  // End
		case 0x50: return 81;  // Down
		case 0x51: return 78;  // Page Down
		case 0x52: return 73;  // Insert
		case 0x53: return 76;  // Delete
		default: return 0;
		}
	}

	/// The key's SDL keycode (what Minecraft's shortcuts like Ctrl+V look at): the character it types unshifted in the
	/// player's keyboard layout, or the scancode flagged as a non-character key.
	int sdl_keycode(DWORD vk, int sc)
	{
		switch (vk)
		{
		case VK_RETURN: return 13;
		case VK_ESCAPE: return 27;
		case VK_BACK: return 8;
		case VK_TAB: return 9;
		case VK_SPACE: return 32;
		case VK_DELETE: return 127;
		default: break;
		}
		const UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFF;
		if (ch > 0x20)
			return int(towlower(wchar_t(ch)));
		return sc | (1 << 30);
	}

	/// A key while Minecraft shows a screen (on the keyboard handler's thread): the key itself, and the character it
	/// types in the player's own keyboard layout.
	void screen_key(DWORD vk, BYTE scan, BOOL extended, BOOL wasDown, BOOL up)
	{
		const int sc = hid_scancode(scan, extended);
		if (sc != 0)
			sendf("{\"t\":\"rkey\",\"sc\":%d,\"kc\":%d,\"m\":%d,\"a\":%d}", sc, sdl_keycode(vk, sc), key_mods(), up ? 0 : wasDown ? -1 : 1);
		if (up)
			return;
		BYTE state[256];
		if (!GetKeyboardState(state))
			return;
		wchar_t text[8];
		const int n = ToUnicodeEx(vk, scan, state, text, 8, 0x4 /* don't disturb dead keys */, GetKeyboardLayout(0));
		for (int i = 0; i < n; ++i)
			if (text[i] >= 0x20 && text[i] != 0x7F)
				sendf("{\"t\":\"chr\",\"c\":%d}", int(text[i]));
	}

	/// While Minecraft shows a screen (inventory, chat, ...): GTA stands still and shows its mouse pointer, and the
	/// pointer, buttons and wheel go to Minecraft (keys go from the keyboard handler). Whether a screen is open.
	int g_screenClosedAt = 0; // (game timer) when a Minecraft screen was last open (0: none lately)

	/// The pause menu's controls off this frame.
	void block_pause_menu()
	{
		for (const int c : {199, 200, 202})
		{
			natives::DisableControlAction(0, c, TRUE);
			natives::DisableControlAction(2, c, TRUE);
		}
	}

	bool g_peek = false; // Alt held over a Minecraft screen: it's hidden, and the player moves and looks about

	bool screen_tick()
	{
		// (left Alt only, over the inventory and other screens, not chat: AltGr, which types @ and the like, is Ctrl + right
		// Alt, and hid the chat being typed in)
		const bool peek = g_screen.load() >= 2 && (GetAsyncKeyState(VK_LMENU) & 0x8000) != 0 && (GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0;
		if (peek != g_peek)
		{
			g_peek = peek;
			sendf("{\"t\":\"peek\",\"on\":%s}", peek ? "true" : "false");
			if (peek)
			{
				compositor::set_cursor(0.0f, 0.0f, false);
				g_cursorX = g_cursorY = -1.0f;
			}
		}
		if (peek)
		{
			block_pause_menu();
			g_screenClosedAt = std::max(1, natives::GetGameTimer());
			return false;
		}
		if (g_screen.load() == 0)
		{
			// the Esc that closed Minecraft's screen (inventory, chat) doesn't go on to open GTA's pause menu: not
			// until it's let go and a moment has passed
			if (g_screenClosedAt != 0)
			{
				if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0 || natives::GetGameTimer() - g_screenClosedAt < 500)
					block_pause_menu();
				else
					g_screenClosedAt = 0;
			}
			if (g_cursorX >= 0.0f)
			{
				compositor::set_cursor(0.0f, 0.0f, false);
				g_cursorX = g_cursorY = -1.0f;
			}
			g_mouseLeft = g_mouseRight = false;
			return false;
		}
		natives::DisableAllControlActions(0);
		block_pause_menu(); // (Esc closes Minecraft's screen instead)
		g_screenClosedAt = std::max(1, natives::GetGameTimer());
		natives::SetMouseCursorThisFrame();
		const float x = std::clamp(natives::GetDisabledControlNormal(0, 239), 0.0f, 1.0f);
		const float y = std::clamp(natives::GetDisabledControlNormal(0, 240), 0.0f, 1.0f);
		if (std::fabs(x - g_cursorX) + std::fabs(y - g_cursorY) > 1e-4f)
		{
			sendf("{\"t\":\"cursor\",\"x\":%.5f,\"y\":%.5f}", x, y);
			g_cursorX = x;
			g_cursorY = y;
		}
		compositor::set_cursor(x, y, true);
		const bool left = natives::IsDisabledControlPressed(0, 24) || natives::IsDisabledControlPressed(0, 237);
		const bool right = natives::IsDisabledControlPressed(0, 25) || natives::IsDisabledControlPressed(0, 238);
		if (left != g_mouseLeft)
		{
			sendf("{\"t\":\"mbtn\",\"b\":1,\"down\":%s,\"m\":%d}", left ? "true" : "false", key_mods());
			g_mouseLeft = left;
		}
		if (right != g_mouseRight)
		{
			sendf("{\"t\":\"mbtn\",\"b\":3,\"down\":%s,\"m\":%d}", right ? "true" : "false", key_mods());
			g_mouseRight = right;
		}
		if (natives::IsDisabledControlJustPressed(0, 15))
			g_ws.send("{\"t\":\"wheel\",\"d\":1}");
		if (natives::IsDisabledControlJustPressed(0, 14))
			g_ws.send("{\"t\":\"wheel\",\"d\":-1}");
		return true;
	}

	/// GTA's HUD stays (minimap, mission text, help); Minecraft's world never covers the minimap (bottom left, placed
	/// by GTA's safe zone and aspect ratio), and only while GTA really draws it: in cutscenes, character switches,
	/// loading screens and anywhere else it's gone, Minecraft shows there too. The director can hide it all for video.
	void hud_tick(bool scene)
	{
		if (!g_showHud)
		{
			natives::HideHudAndRadarThisFrame();
			natives::TheFeedHideThisFrame();
			natives::HideHelpTextThisFrame();
		}
		if (!g_settings.keepMinimap || !g_showHud || scene || g_noInput || natives::IsRadarHidden() || natives::IsHudHidden() ||
			!natives::IsMinimapRendering())
		{
			compositor::set_hud_mask(0.0f, 0.0f, 0.0f, 0.0f);
			return;
		}
		const float margin = (1.0f - natives::GetSafeZoneSize()) * 0.5f;
		const float w = 1.0f / (4.0f * std::max(natives::GetAspectRatio(), 1.0f)), h = 1.0f / 5.674f;
		// the map and the health and armour bars under it (and its north marker over the top edge)
		compositor::set_hud_mask(std::max(0.0f, margin - 0.004f), 1.0f - margin - h - 0.02f, margin + w + 0.008f, 1.0f - margin + 0.012f);
	}

	// ---- Minecraft's movement (see Walk and Proxy) ----

	int64_t cell2_key(int i, int j)
	{
		return (int64_t(i) << 32) ^ uint32_t(j);
	}

	/// A probe through GTA's world from (x0, y0, z0) to (x1, y1, z1): the first surface that isn't in a cell dug out
	/// (a hole's own sides are Minecraft's blocks), its entity (0: the map) and its material. True on a hit.
	/// Whether GTA object `e` is a door that isn't locked (Minecraft's movement walks through it, pushing it open, as
	/// GTA's player would; a locked one stays a wall). Remembered a couple of seconds (a mission may lock it later).
	bool g_probeDoorsOpen = false; // (world_probe: unlocked doors aren't in the way, for the player's own collision)
	bool passable_door(Entity e)
	{
		static std::unordered_map<Entity, std::pair<bool, int>> known;
		const int now = natives::GetGameTimer();
		if (const auto it = known.find(e); it != known.end() && now - it->second.second < 2000)
			return it->second.first;
		if (known.size() > 256)
			known.clear();
		bool open = false;
		if (natives::DoesEntityExist(e) && natives::GetEntityType(e) == 3 && !g_props.handles.count(e))
		{
			const Hash model = natives::GetEntityModel(e);
			const Vector3 o = natives::GetEntityCoords(e, FALSE);
			BOOL locked = 2;
			float heading = -999.0f;
			natives::GetStateOfClosestDoorOfType(model, o.x, o.y, o.z, &locked, &heading);
			open = heading != -999.0f && locked == FALSE;
		}
		known[e] = {open, now};
		return open;
	}

	bool world_probe(Entity ignore, float x0, float y0, float z0, float x1, float y1, float z1, int flags, int options, Vector3 &at,
		Vector3 &normal, Entity &entity, Hash &material)
	{
		const float dx = x1 - x0, dy = y1 - y0, dz = z1 - z0, len = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (len < 1e-4f)
			return false;
		const float ux = dx / len, uy = dy / len, uz = dz / len;
		float sx = x0, sy = y0, sz = z0;
		for (int i = 0; i < 6; ++i)
		{
			BOOL hit = FALSE;
			entity = 0;
			material = 0;
			const int probe = natives::StartShapeTestLosProbe(sx, sy, sz, x1, y1, z1, flags, ignore, options);
			if (natives::GetShapeTestResultIncludingMaterial(probe, &hit, &at, &normal, &material, &entity) != 2 || !hit)
				return false;
			// GTA's water surface isn't a floor or a wall (Minecraft swims in it: stood on, it was walked on and built on)
			const bool water = material == kWaterMaterial;
			if (!water && !(g_probeDoorsOpen && entity != 0 && passable_door(entity)))
				return true;
			sx = at.x + ux * 0.05f;
			sy = at.y + uy * 0.05f;
			sz = at.z + uz * 0.05f;
			if ((x1 - sx) * ux + (y1 - sy) * uy + (z1 - sz) * uz <= 0.0f)
				return false;
		}
		return false;
	}

	/// A probe through GTA's world for the player's own collision: the map, vehicles, objects and glass (see-through
	/// fences and glass block the way like walls do). Not people, not the props for Minecraft's own blocks (Minecraft
	/// has those), and not a door that isn't locked (walked through, pushed open). True on a hit.
	bool solid_probe(Ped ped, float x0, float y0, float z0, float x1, float y1, float z1, Vector3 &at, Vector3 &normal)
	{
		Entity entity = 0;
		Hash material = 0;
		g_probeDoorsOpen = true;
		const bool hit = world_probe(ped, x0, y0, z0, x1, y1, z1, 1 | 2 | 16 | 64, 4, at, normal, entity, material);
		g_probeDoorsOpen = false;
		if (!hit)
			return false;
		return entity == 0 || !g_props.handles.count(entity);
	}

	/// A wall a probe hit at `at` (facing `n`), seen from feet height z: a box just behind its face, from below the
	/// feet up to its top. Close by, the top is measured (a low wall can be jumped onto); further off it's high.
	void wall_add(Ped ped, const Vector3 &at, const Vector3 &n, float z, int now, float half = -1.0f)
	{
		float nx = n.x, ny = n.y;
		const float nl = std::sqrt(nx * nx + ny * ny);
		if (nl < 0.5f)
			return; // a slope or a step's top, not a wall (the floor cells have it)
		nx /= nl;
		ny /= nl;
		const float dist = std::sqrt((at.x - g_walk.x) * (at.x - g_walk.x) + (at.y - g_walk.y) * (at.y - g_walk.y));
		float top = z + 3.0f;
		if (dist < 2.0f)
		{
			Vector3 t = {}, tn = {};
			const float px = at.x - nx * 0.08f, py = at.y - ny * 0.08f;
			if (solid_probe(ped, px, py, z + 3.0f, px, py, z - 0.5f, t, tn) && t.z > at.z - 0.05f)
				top = t.z;
		}
		if (half < 0.0f)
			half = std::clamp(dist * 0.2f + 0.06f, 0.12f, 0.45f);
		const float depth = 0.3f;
		HostBox b = {};
		if (std::fabs(nx) >= std::fabs(ny))
		{
			b.x0 = nx > 0.0f ? at.x - depth : at.x;
			b.x1 = nx > 0.0f ? at.x : at.x + depth;
			b.y0 = at.y - half;
			b.y1 = at.y + half;
		}
		else
		{
			b.y0 = ny > 0.0f ? at.y - depth : at.y;
			b.y1 = ny > 0.0f ? at.y : at.y + depth;
			b.x0 = at.x - half;
			b.x1 = at.x + half;
		}
		b.z0 = z - 1.0f;
		b.z1 = top;
		b.at = now;
		const int64_t key = cell2_key(int(std::floor(at.x * 4.0f)), int(std::floor(at.y * 4.0f))) * 2 + (at.z - z > 1.1f ? 1 : 0);
		g_proxy.walls[key] = b;
		g_proxy.dirty = true;
	}

	/// Two neighbouring rays hit the same wall (alike normals, close together): fill in the wall between them too, so
	/// there's no gap between the two boxes for the player to slip through.
	void wall_join(Ped ped, const Vector3 &a, const Vector3 &na, const Vector3 &b, const Vector3 &nb, float z, int now)
	{
		const float la = std::sqrt(na.x * na.x + na.y * na.y), lb = std::sqrt(nb.x * nb.x + nb.y * nb.y);
		if (la < 0.5f || lb < 0.5f || (na.x * nb.x + na.y * nb.y) / (la * lb) < 0.85f || std::fabs(a.z - b.z) > 0.6f)
			return;
		const float dx = b.x - a.x, dy = b.y - a.y, gap = std::sqrt(dx * dx + dy * dy);
		if (gap < 0.2f || gap > 1.6f)
			return;
		Vector3 n = {};
		n.x = na.x / la + nb.x / lb;
		n.y = na.y / la + nb.y / lb;
		const int steps = int(std::ceil(gap / 0.25f));
		for (int i = 1; i < steps; ++i)
		{
			const float t = float(i) / float(steps);
			Vector3 at = {};
			at.x = a.x + dx * t;
			at.y = a.y + dy * t;
			at.z = a.z + (b.z - a.z) * t;
			wall_add(ped, at, n, z, now, 0.16f);
		}
	}

	/// GTA's collision around the player for Minecraft ("hc": boxes, Minecraft coordinates): the floor cells due a
	/// probe (the ones under the feet every frame, new ones next, then the oldest), a ring of wall probes (shin, waist and
	/// head height in turn) and ones along the way the player moves, the ceiling. `all`: everything at once (Minecraft is
	/// about to take over).
	void proxy_tick(Ped ped, bool all)
	{
		const int now = natives::GetGameTimer();
		const float x = g_walk.x, y = g_walk.y, z = g_walk.z;
		// floors: probed down from above the height the player last stood at (a jump doesn't lift the probes over a
		// low ceiling) to well below the feet (further while falling fast)
		const float base = g_walk.standZ;
		if (std::fabs(base - g_proxy.floorBase) > 0.3f)
		{
			// a new standing height (stairs, a ramp, a drop): every cell is probed again from there, nearest first, and
			// keeps its old floor until then (clearing them left the player over nothing for a few frames: it fell)
			for (auto &[key, cell] : g_proxy.floor)
				cell.at = INT_MIN + 1;
			g_proxy.floorBase = base;
		}
		const float from = base + 1.6f, to = std::min(base, z) - 4.0f + std::min(0.0f, g_drive.vel.z * 0.4f);
		const int ci = int(std::floor(x / kCell)), cj = int(std::floor(y / kCell));
		struct Due
		{
			int at, i, j, d2;
		};
		std::vector<Due> due;
		for (int i = ci - kCellRadius; i <= ci + kCellRadius; ++i)
			for (int j = cj - kCellRadius; j <= cj + kCellRadius; ++j)
			{
				const int d2 = (i - ci) * (i - ci) + (j - cj) * (j - cj);
				const auto it = g_proxy.floor.find(cell2_key(i, j));
				if (it == g_proxy.floor.end() || d2 <= 2)
					due.push_back({INT_MIN, i, j, d2}); // new, or under the feet (every frame)
				else if (now - it->second.at > 250)
					due.push_back({it->second.at, i, j, d2});
			}
		std::sort(due.begin(), due.end(), [](const Due &a, const Due &b) { return a.at != b.at ? a.at < b.at : a.d2 < b.d2; });
		const size_t budget = all ? due.size() : std::min<size_t>(due.size(), 36);
		for (size_t k = 0; k < budget; ++k)
		{
			const float cx = (due[k].i + 0.5f) * kCell, cy = (due[k].j + 0.5f) * kCell;
			Vector3 at = {}, n = {};
			const float top = solid_probe(ped, cx, cy, from, cx, cy, to, at, n) ? at.z : NAN;
			const auto old = g_proxy.floor.find(cell2_key(due[k].i, due[k].j));
			if (old == g_proxy.floor.end() || std::isnan(old->second.top) != std::isnan(top) ||
				(!std::isnan(top) && std::fabs(old->second.top - top) > 0.005f))
				g_proxy.dirty = true;
			g_proxy.floor[cell2_key(due[k].i, due[k].j)] = {top, now, due[k].i, due[k].j};
		}
		// walls: rays out to 2.5 m around the player, at shin (just over what Minecraft steps up), waist and head height
		// in turn; neighbouring hits on the same wall are joined
		constexpr int kRays = 24;
		static const float kHeights[3] = {0.7f, 1.15f, 1.65f};
		g_proxy.ring = (g_proxy.ring + 1) % 3;
		for (int r = 0; r < 3; ++r)
		{
			if (!all && r != g_proxy.ring)
				continue;
			const float hz = z + kHeights[r];
			bool prevHit = false, firstHit = false;
			Vector3 prevAt = {}, prevN = {}, firstAt = {}, firstN = {};
			for (int k = 0; k < kRays; ++k)
			{
				const float a = (float(k) + 0.5f * float(r & 1)) * (6.2831853f / float(kRays)), dx = std::cos(a), dy = std::sin(a);
				Vector3 at = {}, n = {};
				const bool hit = solid_probe(ped, x, y, hz, x + dx * 2.5f, y + dy * 2.5f, hz, at, n);
				if (hit)
				{
					wall_add(ped, at, n, z, now);
					if (prevHit)
						wall_join(ped, prevAt, prevN, at, n, z, now);
					if (k == 0)
					{
						firstHit = true;
						firstAt = at;
						firstN = n;
					}
					else if (k == kRays - 1 && firstHit)
						wall_join(ped, at, n, firstAt, firstN, z, now);
					prevAt = at;
					prevN = n;
				}
				prevHit = hit;
			}
		}
		// along the way the player moves (and a little to each side): what it's about to walk into, before it's there
		const float vx = g_drive.vel.x, vy = g_drive.vel.y, speed = std::sqrt(vx * vx + vy * vy);
		if (speed > 0.5f)
		{
			const float reach = 0.6f + speed * 0.3f;
			for (const float turn : {-0.45f, 0.0f, 0.45f})
			{
				const float c = std::cos(turn), sn = std::sin(turn);
				const float dx = (vx * c - vy * sn) / speed, dy = (vx * sn + vy * c) / speed;
				for (const float hz : {0.7f, 1.5f})
				{
					Vector3 at = {}, n = {};
					if (solid_probe(ped, x, y, z + hz, x + dx * reach, y + dy * reach, z + hz, at, n))
						wall_add(ped, at, n, z, now);
				}
			}
		}
		// the ceiling: over the head, and one spot around it a frame
		static const float kAround[5][2] = {{0.0f, 0.0f}, {0.3f, 0.0f}, {-0.3f, 0.0f}, {0.0f, 0.3f}, {0.0f, -0.3f}};
		g_proxy.around = g_proxy.around % 4 + 1;
		for (int k = 0; k < 5; ++k)
		{
			if (!all && k != 0 && k != g_proxy.around)
				continue;
			const float ox = x + kAround[k][0], oy = y + kAround[k][1];
			Vector3 at = {}, n = {};
			if (solid_probe(ped, ox, oy, z + 1.0f, ox, oy, z + 3.6f, at, n))
			{
				g_proxy.ceilings[cell2_key(int(std::floor(at.x * 4.0f)), int(std::floor(at.y * 4.0f)))] =
					{at.x - 0.35f, at.y - 0.35f, at.z, at.x + 0.35f, at.y + 0.35f, at.z + 1.0f, now};
				g_proxy.dirty = true;
			}
		}
		// walls and ceilings not seen again for a while are gone (an opened door, a car that drove off); far ones too
		for (auto it = g_proxy.walls.begin(); it != g_proxy.walls.end();)
		{
			const float wx = (it->second.x0 + it->second.x1) * 0.5f - x, wy = (it->second.y0 + it->second.y1) * 0.5f - y;
			if (now - it->second.at > 900 || wx * wx + wy * wy > 4.0f * 4.0f)
			{
				it = g_proxy.walls.erase(it);
				g_proxy.dirty = true;
			}
			else
				++it;
		}
		for (auto it = g_proxy.ceilings.begin(); it != g_proxy.ceilings.end();)
			if (now - it->second.at > 600)
			{
				it = g_proxy.ceilings.erase(it);
				g_proxy.dirty = true;
			}
			else
				++it;
		if (!g_proxy.dirty || (!all && now < g_proxy.nextSendAt))
			return;
		g_proxy.nextSendAt = now + 30;
		g_proxy.dirty = false;
		std::string out = "{\"t\":\"hc\",\"b\":[";
		bool first = true;
		auto add = [&](float x0, float y0, float z0, float x1, float y1, float z1) {
			char e[128];
			snprintf(e, sizeof(e), "%s%.2f,%.2f,%.2f,%.2f,%.2f,%.2f", first ? "" : ",", x0, z0 + g_yOffset, -y1, x1, z1 + g_yOffset, -y0);
			out += e;
			first = false;
		};
		for (auto it = g_proxy.floor.begin(); it != g_proxy.floor.end();)
		{
			const FloorCell &c = it->second;
			if (std::abs(c.i - ci) > kCellRadius + 1 || std::abs(c.j - cj) > kCellRadius + 1)
			{
				it = g_proxy.floor.erase(it);
				continue;
			}
			if (!std::isnan(c.top))
				add(c.i * kCell, c.j * kCell, c.top - 1.2f, (c.i + 1) * kCell, (c.j + 1) * kCell, c.top);
			++it;
		}
		for (const auto &[k, b] : g_proxy.walls)
			add(b.x0, b.y0, b.z0, b.x1, b.y1, b.z1);
		for (const auto &[k, b] : g_proxy.ceilings)
			add(b.x0, b.y0, b.z0, b.x1, b.y1, b.z1);
		out += "]}";
		g_ws.send(out);
	}

	/// Put Minecraft's player where GTA's is (g_walk's feet); keepY: only across (Minecraft keeps its height and fall).
	/// Minecraft's positions count again once it says it got this one.
	void walk_correct(bool keepY, const char *why)
	{
		// (logged at most twice a second per reason: walking into a wall corrects ten times a second)
		static const char *lastWhy = nullptr;
		static int lastAt = -100000, skipped = 0;
		const int now = natives::GetGameTimer();
		if (why != lastWhy || now - lastAt > 500)
		{
			logf("pset %s at %.2f %.2f %.2f%s (%d more before)", why, g_walk.x, g_walk.y, g_walk.z, keepY ? " keepY" : "", skipped);
			lastWhy = why;
			lastAt = now;
			skipped = 0;
		}
		else
			++skipped;
		++g_walk.psetId;
		// (Minecraft's position from before it got this doesn't count: taken for where the player is, a teleport of over
		// 20 m looked like Minecraft running off, was corrected again every frame, and Minecraft never caught up)
		g_drive.havePos = false;
		sendf("{\"t\":\"pset\",\"id\":%d,\"pos\":[%.3f,%.3f,%.3f],\"keepY\":%s}", g_walk.psetId, g_walk.x, g_walk.z + g_yOffset, -g_walk.y,
			keepY ? "true" : "false");
	}

	/// Minecraft's movement on or off. On: GTA's player freezes where it stands, GTA's collision around it goes to
	/// Minecraft, and Minecraft's player starts from there. Off: GTA's player is let go where Minecraft's player was.
	void walk_set(Ped ped, bool want)
	{
		if (want == g_walk.on)
			return;
		g_walk.on = want;
		g_walk.acked = false;
		if (!want)
		{
			natives::FreezeEntityPosition(ped, FALSE);
			g_walk.frozen = 0;
			return;
		}
		const Vector3 p = natives::GetEntityCoords(ped, TRUE);
		g_walk.x = p.x;
		g_walk.y = p.y;
		g_walk.z = g_walk.standZ = p.z - 1.0f;
		g_walk.putX = p.x;
		g_walk.putY = p.y;
		g_walk.putZ = p.z;
		g_walk.ground = true;
		g_drive.havePos = false;
		g_drive.vel = {};
		natives::FreezeEntityPosition(ped, TRUE);
		g_walk.frozen = ped;
		g_proxy.floor.clear();
		g_proxy.walls.clear();
		g_proxy.ceilings.clear();
		g_proxy.floorBase = -100000.0f;
		proxy_tick(ped, true);
		// (a riptide's flight has begun in Minecraft from where GTA's player is: putting it there again would stop it)
		if (natives::GetGameTimer() >= g_walk.forceUntil)
			walk_correct(false, "movement on");
		else
			g_walk.acked = true;
	}

	/// Whether Minecraft should move the player now: on foot, in control, and none of what GTA does itself.
	bool walk_wanted(Ped ped)
	{
		const bool forced = natives::GetGameTimer() < g_walk.forceUntil; // (a riptide flight: Minecraft's, even with F6 off)
		if ((!g_mcMove && !forced) || g_gtaHands || g_drive.on || g_carFly.on || (natives::GetGameTimer() < g_walk.gtaUntil && !forced))
			return false;
		if (!natives::IsPlayerControlOn(natives::PlayerId()) || (!natives::IsPedOnFoot(ped) && !natives::IsPedSwimming(ped)) ||
			natives::IsPedInAnyVehicle(ped, FALSE) ||
			natives::IsPedGettingIntoAVehicle(ped))
			return false;
		if (g_mcSpectator)
			return true; // a spectator flies through everything, whatever GTA's player was doing
		if (g_walk.on || forced)
			return true; // (a riptide from GTA's skydive, a ragdoll, a ledge: Minecraft's flight all the same) // (once on, only the above end it: GTA thinking its frozen player falls, climbs or vaults when it's
			             // bridging high over a gap mustn't drop the player into GTA's movement mid-air)
		if (natives::IsPedRagdoll(ped) || natives::IsPedGettingUp(ped) || natives::IsPedClimbing(ped) || natives::IsPedInCover(ped) ||
			natives::GetPedParachuteState(ped) > 0 || natives::IsPedInParachuteFreeFall(ped))
			return false;
		// a ladder, getting out of a car, a vault, getting in, cover
		for (const int task : {1, 2, 47, 48, 50, 160, 287, 421})
			if (natives::GetIsTaskActive(ped, task))
				return false;
		// (GTA's water is Minecraft's too now: Minecraft swims in it, as in its own)
		return true;
	}

	/// Minecraft's player became a spectator (/spectator) or stopped being one (/creative): GTA's player flies through
	/// everything with it, unseen, untouchable and ignored by everyone; or back to GTA's rules.
	void spectator_apply(Ped ped)
	{
		if (g_mcSpectator == g_spectatorOn)
			return;
		g_spectatorOn = g_mcSpectator;
		const Player player = natives::PlayerId();
		natives::SetEntityCollision(ped, g_mcSpectator ? FALSE : TRUE, TRUE);
		natives::SetEveryoneIgnorePlayer(player, g_mcSpectator ? TRUE : FALSE);
		natives::SetPoliceIgnorePlayer(player, g_mcSpectator ? TRUE : FALSE);
		if (g_mcSpectator)
		{
			natives::SetEntityInvincible(ped, TRUE);
			natives::Notify("Spectator: fly through everything (~y~Space~s~ up, ~y~C~s~ down). ~y~/creative~s~ to come back");
		}
		else
			apply_rules(ped);
	}

	/// The movement keys for Minecraft, from GTA's own controls (a pad works too); GTA's player doesn't get them.
	/// Bits: 1 forward, 2 back, 4 left, 8 right, 16 jump (Space; flying: up), 32 sneak (C, GTA's look-behind key;
	/// flying: down), 64 sprint (Shift, GTA's own sprint key).
	// Stamina (Minecraft's hunger bar shows it): sprinting and swimming hard use it up, resting brings it back. Run out
	// and Minecraft's sprint is off until it's back to a third.

	void stamina_tick(Ped ped, int input)
	{
		const float dt = std::clamp(natives::GetFrameTime(), 0.0f, 0.1f);
		const bool moving = (input & 15) != 0;
		const bool hard = (g_walk.on ? (input & 64) != 0 && moving : natives::IsPedSprinting(ped) != FALSE) ||
			(natives::IsPedSwimming(ped) && natives::GetEntitySpeed(ped) > 2.0f);
		const int now = natives::GetGameTimer();
		if (hard && !g_carFly.on && !natives::IsPedInAnyVehicle(ped, FALSE))
		{
			g_stamina = std::max(0.0f, g_stamina - dt * 100.0f / 14.0f); // ~14 s of sprinting
			g_staminaRestAt = now + 900;
		}
		else if (now >= g_staminaRestAt)
			g_stamina = std::min(100.0f, g_stamina + dt * 100.0f / 7.0f);
		if (g_stamina <= 0.0f)
			g_exhausted = true;
		else if (g_stamina >= 33.0f)
			g_exhausted = false;
		if (g_exhausted && !g_walk.on && !g_cheats.fastRun)
			natives::DisableControlAction(0, 21, TRUE); // (out of breath: GTA's sprint waits too)
	}

	int walk_input(bool screen)
	{
		for (const int c : {21, 22, 26, 30, 31, 32, 33, 34, 35, 36, 55})
			natives::DisableControlAction(0, c, TRUE);
		if (screen)
			return 0;
		const float lr = natives::GetDisabledControlNormal(0, 30), ud = natives::GetDisabledControlNormal(0, 31);
		int in = 0;
		if (ud < -0.4f || natives::IsDisabledControlPressed(0, 32))
			in |= 1;
		if (ud > 0.4f || natives::IsDisabledControlPressed(0, 33))
			in |= 2;
		if (lr < -0.4f || natives::IsDisabledControlPressed(0, 34))
			in |= 4;
		if (lr > 0.4f || natives::IsDisabledControlPressed(0, 35))
			in |= 8;
		if (natives::IsDisabledControlPressed(0, 22))
			in |= 16;
		if (natives::IsDisabledControlPressed(0, 26))
			in |= 32;
		if (natives::IsDisabledControlPressed(0, 21) && !g_exhausted)
			in |= 64;
		return in;
	}

	/// Where the player looks: Minecraft's free look while it's on, else GTA's gameplay camera (pitch, roll, heading).
	Vector3 look_rot()
	{
		if (g_walkCam.cam == 0)
			return natives::GetGameplayCamRot(2);
		Vector3 r = {};
		r.x = g_walkCam.pitch;
		r.z = g_walkCam.heading;
		return r;
	}

	/// Space held (not tapped: a tap is always Minecraft's own jump) at a ledge too high for Minecraft's jump but not
	/// for GTA's climb (a wall, a fence, a container): GTA climbs it. True if it does.
	bool walk_climb(Ped ped)
	{
		const int now = natives::GetGameTimer();
		if (natives::IsDisabledControlJustPressed(0, 22))
			g_spaceDownAt = now;
		if (!natives::IsDisabledControlPressed(0, 22))
			g_spaceDownAt = INT_MAX;
		if (g_spaceDownAt == INT_MAX || now - g_spaceDownAt < 350)
			return false;
		// (only from the ground, or the jump just taken from it: never a climb out of a fall, a jump in mid-air)
		if (!g_walk.ground && (now - g_walk.groundAt > 450 || g_walk.z < g_walk.standZ - 0.3f))
			return false;
		const Vector3 r = look_rot();
		const float h = r.z * 3.14159265f / 180.0f, fx = -std::sin(h), fy = std::cos(h);
		// measured from where the player last stood (Minecraft's jumps go on while Space is held)
		const float x = g_walk.x, y = g_walk.y, z = g_walk.standZ;
		Vector3 wall = {}, n = {}, top = {}, room = {};
		if (!solid_probe(ped, x, y, z + 1.1f, x + fx * 1.0f, y + fy * 1.0f, z + 1.1f, wall, n))
			return false;
		const float tx = wall.x + fx * 0.3f, ty = wall.y + fy * 0.3f;
		if (!solid_probe(ped, tx, ty, z + 3.3f, tx, ty, z + 0.5f, top, n))
			return false;
		const float rise = top.z - z;
		if (rise < 1.3f || rise > 3.1f || solid_probe(ped, tx, ty, top.z + 0.1f, tx, ty, top.z + 1.0f, room, n))
			return false; // Minecraft's jump does lower ones; nothing climbs higher; or no room up there
		g_spaceDownAt = INT_MAX;
		walk_set(ped, false);
		natives::SetEntityCoordsNoOffset(ped, g_walk.x, g_walk.y, g_walk.standZ + 1.0f); // (from the ground, not mid-jump)
		natives::SetEntityHeading(ped, r.z);
		natives::TaskClimb(ped);
		g_walk.gtaUntil = now + 1200;
		return true;
	}

	/// F with no car close by: maybe a ladder. GTA gets the player a moment to find one and climb it its own way.
	bool walk_ladder(Ped ped)
	{
		if (!natives::IsDisabledControlJustPressed(0, 23))
			return false;
		bool car = false;
		each_vehicle_near(g_walk.x, g_walk.y, g_walk.z + 1.0f, 6.0f, [&](Vehicle, float, float, float) { car = true; });
		if (car)
			return false; // GTA gets in itself (and moves the player for that: walk_wanted lets it)
		walk_set(ped, false);
		natives::TaskClimbLadder(ped);
		g_walk.gtaUntil = natives::GetGameTimer() + 900;
		return true;
	}

	/// A car about to run into the player: GTA's physics should knock the player down, not a frozen player stop the car.
	bool car_coming(float x, float y, float z)
	{
		bool coming = false;
		each_vehicle_near(x, y, z, 15.0f, [&](Vehicle v, float dx, float dy, float) {
			const Vector3 vel = natives::GetEntityVelocity(v);
			const float speed = std::sqrt(vel.x * vel.x + vel.y * vel.y);
			if (coming || speed < 3.0f)
				return;
			const float ux = vel.x / speed, uy = vel.y / speed;
			const float ahead = -(dx * ux + dy * uy), side = std::fabs(-dx * uy + dy * ux);
			coming = ahead > 0.0f && side < 2.0f && ahead / speed < 0.6f;
		});
		return coming;
	}

	/// Minecraft's player went from GTA's last spot to (tx, ty, tz): if a wall of GTA's is in the way (one Minecraft
	/// didn't know about yet), stop short of it (Minecraft knows it from now on). True if it was stopped.
	bool walk_blocked(Ped ped, float &tx, float &ty, float tz)
	{
		const float dx = tx - g_walk.x, dy = ty - g_walk.y, len = std::sqrt(dx * dx + dy * dy);
		if (len < 0.01f)
			return false;
		const float ux = dx / len, uy = dy / len;
		for (const float h : {0.7f, 1.15f, 1.65f})
		{
			Vector3 at = {}, n = {};
			// (the player is 0.6 m wide: look 0.3 m past where it got to)
			if (!solid_probe(ped, g_walk.x, g_walk.y, g_walk.z + h, tx + ux * 0.3f, ty + uy * 0.3f, tz + h, at, n) ||
				n.x * n.x + n.y * n.y < 0.25f)
				continue; // nothing, or a slope or a step (Minecraft's step-up has those)
			wall_add(ped, at, n, g_walk.z, natives::GetGameTimer());
			const float along = std::max(0.0f, (at.x - g_walk.x) * ux + (at.y - g_walk.y) * uy - 0.32f);
			tx = g_walk.x + ux * along;
			ty = g_walk.y + uy * along;
			return true;
		}
		return false;
	}

	/// GTA's player put at (x, y, z) while Minecraft moves it (walk_tick tells that from a mission moving it).
	void walk_put(Ped ped, float x, float y, float z)
	{
		natives::SetEntityCoordsNoOffset(ped, x, y, z);
		g_walk.putX = x;
		g_walk.putY = y;
		g_walk.putZ = z;
	}

	/// GTA's pickups (money dropped, weapons, health, armour) where Minecraft's player walks: GTA's frozen player
	/// doesn't collect them by itself, so it's let go and put on the pickup for a moment, and GTA collects it.
	void pickups_tick(Ped ped, int now)
	{
		static int nextScan = 0, holdUntil = 0, triedSince = 0;
		static Object target = 0, tried = 0;
		static std::set<Object> refused; // (not collectable: full ammo, a mission's)
		if (now >= nextScan)
		{
			nextScan = now + 150;
			target = 0;
			if (!g_mcSpectator)
			{
				float best = 1.8f * 1.8f;
				int handles[256];
				const int n = worldGetAllObjects(handles, 256);
				for (int i = 0; i < n; ++i)
				{
					const Object o = handles[i];
					if (g_props.handles.count(o) || refused.count(o))
						continue;
					const Vector3 at = natives::GetEntityCoords(o, TRUE);
					const float dx = at.x - g_walk.x, dy = at.y - g_walk.y, dz = at.z - g_walk.z;
					const float d2 = dx * dx + dy * dy;
					if (d2 < best && dz > -0.8f && dz < 2.0f && (natives::IsObjectAPickup(o) || natives::IsObjectAPortablePickup(o)))
					{
						best = d2;
						target = o;
					}
				}
			}
		}
		if (target != tried)
		{
			tried = target;
			triedSince = now;
		}
		else if (target != 0 && now - triedSince > 1500)
		{
			refused.insert(target);
			if (refused.size() > 64)
				refused.clear();
			target = 0;
		}
		if (target != 0 && natives::DoesEntityExist(target))
		{
			const Vector3 at = natives::GetEntityCoords(target, TRUE);
			natives::FreezeEntityPosition(ped, FALSE);
			walk_put(ped, at.x, at.y, at.z + 0.9f);
			holdUntil = now + 250;
		}
		else if (holdUntil != 0 && now >= holdUntil)
		{
			holdUntil = 0;
			natives::FreezeEntityPosition(ped, TRUE); // (collected, or gone: frozen again where Minecraft's player is)
			walk_put(ped, g_walk.x, g_walk.y, g_walk.z + 1.0f);
		}
		else if (holdUntil != 0)
			walk_put(ped, g_walk.x, g_walk.y, g_walk.z + 1.0f);
	}

	/// A mission restarted (failed and retried) or back at a checkpoint (the player died or was arrested in it): Minecraft
	/// clears its things, as /clearall but GTA's own cars and people stay (they're the mission's). Told once the player is
	/// up again (the reload is behind a faded-out screen).
	void mission_restart_tick(Ped ped)
	{
		static bool wasMission = false, pending = false;
		static int endedAt = -100000;
		const int now = natives::GetGameTimer();
		const bool mission = natives::GetMissionFlag() != FALSE;
		const bool down = natives::IsEntityDead(ped) || natives::IsPlayerBeingArrested(natives::PlayerId());
		if (mission && down)
			pending = true;
		if (wasMission && !mission)
			endedAt = now;
		if (!wasMission && mission && now - endedAt < 20000)
			pending = true; // (a mission failed without dying, and retried: its script starts over)
		wasMission = mission;
		if (pending && !down)
		{
			pending = false;
			logf("mission: restarted or back at a checkpoint: Minecraft's things cleared");
			g_ws.send("{\"t\":\"restart\"}");
		}
	}

	/// GTA's time scale (its slow motion: the player dying, Michael's bullet time, Franklin's driving) to Minecraft, which
	/// ticks that much slower then: its swings, the bow's draw, the attack bar, its mobs. Measured as GTA's frame time
	/// against the real one, over a quarter second, only while one of those is on (a slow frame rate isn't slow motion).
	void timescale_tick(Ped ped)
	{
		static int64_t since = 0;
		static float game = 0.0f;
		static float told = 1.0f;
		const int64_t now = now_nanos();
		if (since == 0)
		{
			since = now;
			return;
		}
		game += natives::GetFrameTime();
		const float real = float(now - since) * 1e-9f;
		if (real < 0.25f)
			return;
		const bool slowable = natives::IsEntityDead(ped) || natives::IsSpecialAbilityActive(natives::PlayerId());
		const float scale = slowable ? std::clamp(game / real, 0.05f, 1.0f) : 1.0f;
		since = now;
		game = 0.0f;
		const float want = scale > 0.92f ? 1.0f : std::round(scale * 20.0f) / 20.0f;
		if (std::fabs(want - told) < 0.04f)
			return;
		told = want;
		logf("timescale: GTA runs at %.2f: Minecraft too", want);
		sendf("{\"t\":\"timescale\",\"s\":%.2f}", want);
	}

	/// Every frame while Minecraft moves the player: GTA's player goes where Minecraft's is (stopped at a wall
	/// Minecraft didn't know yet), faces where the camera looks and pushes doors in its way open; Minecraft gets GTA's
	/// collision. Returns the keys for Minecraft (walk_input).
	int walk_tick(Ped ped, bool screen)
	{
		const int now = natives::GetGameTimer();
		const int in = walk_input(screen);
		// Q: GTA's cover (GTA moves the player into it; Minecraft's movement is back once out of cover)
		if (!screen && !g_mcSpectator && natives::IsDisabledControlJustPressed(0, 44))
		{
			walk_set(ped, false);
			g_walk.gtaUntil = now + 1200;
			natives::SetControlValueNextFrame(0, 44, 1.0f);
			logf("cover: Q, GTA's movement for it");
			return 0;
		}
		const Vector3 cam = look_rot();
		const Vector3 p = natives::GetEntityCoords(ped, TRUE);
		// (measured from where we put it last: on a pickup it was put by us, and taking that for a teleport moved Minecraft's
		// player onto every pickup in reach and stopped it dead there)
		const float mx = p.x - g_walk.putX, my = p.y - g_walk.putY, mz = p.z - g_walk.putZ;
		if (mx * mx + my * my + mz * mz > 1.0f)
		{
			// something else moved GTA's player (a mission's teleport): Minecraft's player goes there too
			g_walk.x = p.x;
			g_walk.y = p.y;
			g_walk.z = g_walk.standZ = p.z - 1.0f;
			walk_correct(false, "GTA moved the player");
		}
		else if (g_walk.acked && g_drive.havePos)
		{
			// where Minecraft's player is now: its last sample carried forward by its velocity (not far: carried into a
			// wall Minecraft stopped it at, GTA's side would see it go through and pull it back)
			const int64_t nowNs = now_nanos();
			const float dt = std::clamp(float(nowNs - g_drive.posNanos) * 1e-9f, 0.0f, 0.08f);
			float tx = g_drive.pos.x + g_drive.vel.x * dt, ty = g_drive.pos.y + g_drive.vel.y * dt;
			float tz = g_drive.pos.z + g_drive.vel.z * dt;
			{
				const float fdt = std::clamp(float(nowNs - g_walk.estAt) * 1e-9f, 0.0f, 0.1f);
				g_walk.estAt = nowNs;
				const float ex = tx - g_walk.ex, ey = ty - g_walk.ey, ez = tz - g_walk.ez;
				if (!g_walk.haveEst || ex * ex + ey * ey + ez * ez > 1.0f)
				{
					g_walk.ex = tx; g_walk.ey = ty; g_walk.ez = tz;
					g_walk.haveEst = true;
				}
				else
				{
					const float ease = 1.0f - std::exp(-fdt * 15.0f);
					g_walk.ex += g_drive.vel.x * fdt; g_walk.ey += g_drive.vel.y * fdt; g_walk.ez += g_drive.vel.z * fdt;
					g_walk.ex += (tx - g_walk.ex) * ease; g_walk.ey += (ty - g_walk.ey) * ease; g_walk.ez += (tz - g_walk.ez) * ease;
				}
				// (on the ground the height is Minecraft's own: steps and slabs mustn't be smoothed into the floor)
				tx = g_walk.ex; ty = g_walk.ey;
				if (!g_walk.ground)
					tz = g_walk.ez;
			}
			const float jx = tx - g_walk.x, jy = ty - g_walk.y, jz = tz - g_walk.z, jump = std::sqrt(jx * jx + jy * jy + jz * jz);
			if (jump > 20.0f && now - g_walk.teleportAt > 3000)
				walk_correct(false, "Minecraft far off"); // Minecraft put its player far away by itself (a respawn): back to GTA's
			else
			{
				if (g_mcSpectator)
				{
					// a spectator goes through walls and floors
				}
				else if (g_mcFlying && jump > 0.05f && jump < 40.0f)
				{
					// gliding or flying: its whole way is checked (fast, so no going through a building between frames)
					Vector3 at = {}, n = {};
					bool blocked = false;
					const float ux = jx / jump, uy = jy / jump, uz = jz / jump;
					for (const float hz : {0.3f, 0.9f, 1.6f})
						if (!blocked && solid_probe(ped, g_walk.x, g_walk.y, g_walk.z + hz, tx + ux * 0.4f, ty + uy * 0.4f, tz + hz + uz * 0.4f, at, n))
						{
							blocked = true;
							const float back = std::sqrt((at.x - g_walk.x) * (at.x - g_walk.x) + (at.y - g_walk.y) * (at.y - g_walk.y) +
								(at.z - hz - g_walk.z) * (at.z - hz - g_walk.z));
							const float keep = std::max(0.0f, back - 0.5f);
							tx = g_walk.x + ux * keep;
							ty = g_walk.y + uy * keep;
							tz = g_walk.z + uz * keep;
						}
					if (blocked)
					{
						// stopped at the wall, as Minecraft's elytra stops at a block (and falls from there)
						g_walk.x = tx;
						g_walk.y = ty;
						g_walk.z = tz;
						walk_correct(false, "flew into a wall");
						g_walk.correctedAt = now;
					}
				}
				else if (jump < 6.0f && walk_blocked(ped, tx, ty, tz) && now - g_walk.correctedAt > 100)
				{
					g_walk.x = tx;
					g_walk.y = ty;
					walk_correct(true, "walked into a wall");
					g_walk.correctedAt = now;
				}
				// falling fast onto a floor: stopped on it (Minecraft's collision for it can come too late at a riptide's or
				// a long fall's speed, and the player went through, wherever it had stood before)
				bool landed = false;
				if (!g_mcSpectator && !g_mcFlying && g_drive.vel.z < -5.0f && tz < g_walk.z - 0.1f && jump < 40.0f)
				{
					Vector3 at = {}, n = {};
					float wt = 0.0f;
					const bool wet = water_surface(tx, ty, tz - 1.0f, g_walk.z + 2.0f, wt) && wt > tz - 0.2f; // (rivers too)
					if (!wet && solid_probe(ped, g_walk.x, g_walk.y, g_walk.z + 0.3f, tx, ty, tz - 0.05f, at, n) && n.z > 0.7f &&
						at.z > tz + 0.05f)
					{
						tx = at.x;
						ty = at.y;
						tz = at.z + 0.02f;
						landed = true;
					}
				}
				g_walk.x = tx;
				g_walk.y = ty;
				g_walk.z = tz;
				if (g_walk.ground || landed)
					g_walk.standZ = tz;
				if (landed)
				{
					g_walk.haveEst = false;
					walk_correct(false, "landed on a floor");
					g_walk.correctedAt = now;
				}
			}
		}
		// (not in water: sinking and diving there is swimming, and GTA's water surface isn't a floor to be put back on)
		float waterTop = 0.0f;
		const bool inWater = water_surface(g_walk.x, g_walk.y, g_walk.z - 1.0f, g_walk.z + 2.0f, waterTop) && waterTop > g_walk.z - 0.2f; // (rivers too)
		{
			static bool was = false;
			if (inWater != was)
				logf("walk: %s GTA's water (surface %.2f, feet %.2f)", inWater ? "in" : "out of", waterTop, g_walk.z);
			was = inWater;
		}
		// falling fast right through a floor this frame (GTA's collision for it came too late): stood back on it. Only a
		// real fall through (deep under a flat floor): on slopes and stairs Minecraft's feet are a little under GTA's
		// surface all the time, and taking that for a fall put the player back up and, twice, ended Minecraft's movement
		if (!g_mcSpectator && !g_mcFlying && !inWater && !g_walk.ground && g_drive.vel.z < -6.0f)
		{
			Vector3 at = {}, n = {};
			if (solid_probe(ped, g_walk.x, g_walk.y, g_walk.z + 1.6f, g_walk.x, g_walk.y, g_walk.z - 0.05f, at, n) && n.z > 0.85f &&
				at.z > g_walk.z + 0.7f && at.z < g_walk.standZ + 0.3f)
			{
				g_walk.z = g_walk.standZ = at.z + 0.02f;
				g_walk.haveEst = false;
				walk_correct(false, "fell through a floor");
			}
		}
		// fell through GTA's floor (its collision came too late, or wasn't found): back up onto it. Twice in a few
		// seconds: GTA's collision isn't working out here, so GTA's own movement takes over
		if (!g_mcSpectator && !inWater && now >= g_walk.nextFellCheck && !g_walk.ground && g_drive.vel.z < -1.0f && g_walk.z < g_walk.standZ - 1.0f)
		{
			g_walk.nextFellCheck = now + 250;
			const auto it = g_proxy.floor.find(cell2_key(int(std::floor(g_walk.x / kCell)), int(std::floor(g_walk.y / kCell))));
			float floor = NAN, ground = 0.0f;
			if (it != g_proxy.floor.end() && !std::isnan(it->second.top) && it->second.top < g_walk.standZ + 0.6f)
				floor = it->second.top;
			else if (natives::GetGroundZFor3dCoord(g_walk.x, g_walk.y, g_walk.standZ + 1.0f, &ground, TRUE, FALSE) && ground != 0.0f &&
				ground < g_walk.standZ + 0.6f)
				floor = ground;
			if (!std::isnan(floor) && floor > g_walk.z + 0.9f)
			{
				// (back up onto it; never GTA's movement instead: that dropped players bridging over gaps)
				g_walk.z = g_walk.standZ = floor + 0.02f;
				g_walk.haveEst = false;
				walk_correct(false, "below the floor");
				g_walk.fellAt = now;
			}
		}
		// fell out of GTA's world (glitched under the map: nothing under, GTA's ground overhead): back up onto that
		// ground, as GTA does with its own movement (Minecraft's void doesn't kill the player meanwhile)
		if (g_walk.ground)
		{
			g_walk.safeX = g_walk.x;
			g_walk.safeY = g_walk.y;
			g_walk.safeZ = g_walk.z;
			g_walk.haveSafe = true;
		}
		if (!g_mcSpectator && !g_mcFlying && !inWater && !g_walk.ground && now >= g_walk.nextVoidCheck &&
			(g_drive.vel.z < -3.0f || g_walk.z + g_yOffset < -60.0f))
		{
			g_walk.nextVoidCheck = now + 300;
			Vector3 at = {}, n = {};
			float top = 0.0f;
			const bool groundOver = natives::GetGroundZFor3dCoord(g_walk.x, g_walk.y, 1500.0f, &top, FALSE, FALSE) && top != 0.0f;
			const bool out = !solid_probe(ped, g_walk.x, g_walk.y, g_walk.z + 0.5f, g_walk.x, g_walk.y, g_walk.z - 60.0f, at, n) &&
				(!groundOver || top > g_walk.z + 3.0f);
			if (!out)
				g_walk.voidSince = 0;
			else if (g_walk.voidSince == 0)
				g_walk.voidSince = now;
			else if (now - g_walk.voidSince > 600)
			{
				if (groundOver)
					g_walk.z = top + 0.05f;
				else if (g_walk.haveSafe)
				{
					g_walk.x = g_walk.safeX;
					g_walk.y = g_walk.safeY;
					g_walk.z = g_walk.safeZ + 0.05f;
				}
				g_walk.standZ = g_walk.z;
				g_walk.haveEst = false;
				g_walk.voidSince = 0;
				walk_correct(false, "fell out of the world");
			}
		}
		// a long fall with a parachute on: GTA's own skydive takes over (F opens the parachute), and lands back in
		// Minecraft's movement. Without one, Minecraft's fall damage it is (or a water bucket under you)
		if (!g_mcSpectator && !g_mcFlying && !inWater && !g_walk.ground && g_drive.vel.z < -12.0f && natives::HasPedGotWeapon(ped, 0xFBAB5776))
		{
			float under = 0.0f;
			if (!natives::GetGroundZFor3dCoord(g_walk.x, g_walk.y, g_walk.z, &under, FALSE, FALSE) || g_walk.z - under > 30.0f)
			{
				const Vector3 v = g_drive.vel;
				walk_put(ped, g_walk.x, g_walk.y, g_walk.z + 1.0f);
				walk_set(ped, false);
				natives::SetEntityVelocity(ped, v.x, v.y, v.z);
				natives::TaskSkyDive(ped, TRUE);
				g_walk.gtaUntil = now + 2500;
				logf("skydive: a long fall (%.1f m/s) with a parachute: GTA's skydive", v.z);
				static bool told = false;
				if (!told)
				{
					told = true;
					natives::Notify("Falling with a parachute: ~y~F~s~ opens it");
				}
				return 0;
			}
		}
		walk_put(ped, g_walk.x, g_walk.y, g_walk.z + 1.0f);
		natives::SetEntityHeading(ped, cam.z);
		natives::SetPedDiesInWater(ped, FALSE); // (Minecraft swims, and keeps its own breath)
		// standing still on GTA's own ground: GTA's player isn't held frozen, only put where Minecraft's is (a mission's
		// "be here" trigger waited for a player that was, and never went off)
		{
			float under = 0.0f;
			const bool still = (in & 15) == 0 && g_walk.ground && !g_mcFlying && !g_mcSpectator &&
				natives::GetGroundZFor3dCoord(g_walk.x, g_walk.y, g_walk.z + 0.5f, &under, FALSE, FALSE) && std::fabs(under - g_walk.z) < 0.3f;
			natives::FreezeEntityPosition(ped, still ? FALSE : TRUE);
		}
		pickups_tick(ped, now);
		if (g_mcSpectator)
			return in;
		// a car about to hit: GTA's physics has the player for a moment (only standing on GTA's own ground: on Minecraft's
		// blocks, or in the air, GTA's movement would drop the player)
		float underZ = 0.0f;
		if (g_walk.ground && !g_mcFlying && natives::GetGroundZFor3dCoord(g_walk.x, g_walk.y, g_walk.z + 0.5f, &underZ, FALSE, FALSE) &&
			std::fabs(underZ - g_walk.z) < 0.35f && car_coming(g_walk.x, g_walk.y, g_walk.z + 1.0f))
		{
			walk_set(ped, false);
			g_walk.gtaUntil = now + 1500;
			return 0;
		}
		// pushing on a door (or anything loose) in the way opens it, as GTA's player would
		if ((in & 15) != 0)
		{
			const float h = cam.z * 3.14159265f / 180.0f;
			const float f = ((in & 1) ? 1.0f : 0.0f) - ((in & 2) ? 1.0f : 0.0f), s = ((in & 8) ? 1.0f : 0.0f) - ((in & 4) ? 1.0f : 0.0f);
			float ux = -std::sin(h) * f + std::cos(h) * s, uy = std::cos(h) * f + std::sin(h) * s;
			const float ul = std::sqrt(ux * ux + uy * uy);
			if (ul > 0.1f)
			{
				ux /= ul;
				uy /= ul;
				BOOL hit = FALSE;
				Vector3 at = {}, n = {};
				Entity e = 0;
				const int probe = natives::StartShapeTestLosProbe(g_walk.x, g_walk.y, g_walk.z + 0.6f, g_walk.x + ux * 0.9f, g_walk.y + uy * 0.9f,
					g_walk.z + 0.6f, 16, ped, 4);
				if (natives::GetShapeTestResult(probe, &hit, &at, &n, &e) == 2 && hit && e != 0 && natives::DoesEntityExist(e) &&
					natives::GetEntityType(e) == 3 && !g_props.handles.count(e))
					natives::ApplyForceToEntity(e, ux * 3.0f, uy * 3.0f, 0.0f); // (a door swings open on its hinges: a push only)
			}
		}
		proxy_tick(ped, false);
		return in;
	}

	/// Who Steve stands in for: the player, or in a cutscene the cutscene's copy of the player's character.
	Ped steve_ped(Ped ped)
	{
		if (!natives::IsCutscenePlaying())
			return ped;
		static const Hash michael = natives::GetHashKey("player_zero"), franklin = natives::GetHashKey("player_one"),
			trevor = natives::GetHashKey("player_two");
		const Hash model = natives::GetEntityModel(ped);
		const char *name = model == michael ? "MICHAEL" : model == franklin ? "FRANKLIN" : model == trevor ? "TREVOR" : nullptr;
		if (name == nullptr)
			return ped;
		const Entity e = natives::GetEntityIndexOfCutsceneEntity(name, model);
		return e != 0 && natives::DoesEntityExist(e) && natives::GetEntityType(e) == 1 ? Ped(e) : ped;
	}

	/// Where the crosshair points in GTA's world (Minecraft coordinates): Steve shoots and throws there, rather than
	/// straight ahead from his own head (beside the camera's line, so arrows would miss by as much as he stands aside).
	void aim_point(Ped ped, const Vector3 &c, const Vector3 &r, float &ax, float &ay, float &az)
	{
		const float d2r = 3.14159265f / 180.0f, h = r.z * d2r, pt = r.x * d2r;
		const float fx = -std::sin(h) * std::cos(pt), fy = std::cos(h) * std::cos(pt), fz = std::sin(pt);
		float tx = c.x + fx * 120.0f, ty = c.y + fy * 120.0f, tz = c.z + fz * 120.0f;
		BOOL hit = FALSE;
		Vector3 end = {}, normal = {};
		Entity entity = 0;
		const int probe = natives::StartShapeTestLosProbe(c.x + fx, c.y + fy, c.z + fz, tx, ty, tz, 1 | 2 | 4 | 16, ped);
		if (natives::GetShapeTestResult(probe, &hit, &end, &normal, &entity) == 2 && hit)
		{
			tx = end.x;
			ty = end.y;
			tz = end.z;
		}
		ax = tx;
		ay = tz + g_yOffset;
		az = -ty;
	}

	/// What the crosshair points at in GTA's world within block reach: the point and the surface's normal (Minecraft
	/// coordinates), so Minecraft can place blocks against GTA's walls and ceilings.
	bool block_point(Ped ped, const Vector3 &c, const Vector3 &r, float out[8])
	{
		const float d2r = 3.14159265f / 180.0f, h = r.z * d2r, pt = r.x * d2r;
		const float fx = -std::sin(h) * std::cos(pt), fy = std::cos(h) * std::cos(pt), fz = std::sin(pt);
		Vector3 at = {}, n = {};
		Entity e = 0;
		Hash material = 0;
		const float reachTo = std::max(14.0f, g_meleeRange + 6.0f); // (the reach, /range, from the camera behind Steve)
		if (!world_probe(ped, c.x + fx * 0.3f, c.y + fy * 0.3f, c.z + fz * 0.3f, c.x + fx * reachTo, c.y + fy * reachTo, c.z + fz * reachTo,
				1 | 2 | 16 | 64, 4, at, n, e, material) ||
			(e != 0 && g_props.handles.count(e)))
			return false;
		out[0] = at.x;
		out[1] = at.z + g_yOffset;
		out[2] = -at.y;
		out[3] = n.x;
		out[4] = n.z;
		out[5] = -n.y;
		// whose it is (0 the map, 2 a car, 3 a thing: a door, a sign)
		out[6] = 0.0f;
		out[7] = e != 0 ? float(natives::GetEntityType(e)) : 0.0f;
		return true;
	}

	/// The keys for Minecraft's flight in a flying car: W A S D (GTA's driving keys), Space up, Ctrl down, Shift faster.
	int car_fly_input()
	{
		for (const int c : {59, 60, 61, 62, 63, 64, 71, 72, 76, 87, 88, 89, 90})
			natives::DisableControlAction(0, c, TRUE);
		int in = 0;
		if (natives::IsDisabledControlPressed(0, 71))
			in |= 1;
		if (natives::IsDisabledControlPressed(0, 72))
			in |= 2;
		if (natives::IsDisabledControlPressed(0, 63))
			in |= 4;
		if (natives::IsDisabledControlPressed(0, 64))
			in |= 8;
		if (natives::IsDisabledControlPressed(0, 76))
			in |= 16;
		if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
			in |= 32;
		if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
			in |= 64;
		return in;
	}

	/// The flying car off: it drives (and falls) again, keeping its speed.
	void car_fly_end()
	{
		if (!g_carFly.on)
			return;
		g_carFly.on = false;
		g_walk.acked = false;
		if (natives::DoesEntityExist(g_carFly.car))
		{
			natives::FreezeEntityPosition(g_carFly.car, FALSE);
			natives::SetEntityVelocity(g_carFly.car, g_drive.vel.x, g_drive.vel.y, g_drive.vel.z);
		}
		g_carFly.car = 0;
	}

	/// In the driver's seat with Minecraft's movement on, Space twice: the car flies with Steve. Minecraft flies the
	/// player (its creative flight, against GTA's collision around it) and the car goes where the player is, turned
	/// where the camera looks; Space twice again (or touching down) and it drives again. Returns the keys for Minecraft.
	bool g_flyAllowed = false; // Minecraft's /fly: flying on (the player, and the car with Space twice) or off

	int car_fly_tick(Ped ped, bool control)
	{
		const int now = natives::GetGameTimer();
		const Vehicle car = natives::IsPedInAnyVehicle(ped, FALSE) ? natives::GetVehiclePedIsIn(ped, FALSE) : 0;
		const bool driver = car != 0 && natives::GetPedInVehicleSeat(car, -1) == ped;
		bool toggle = false;
		// (taking off only slowly: a handbrake tapped twice while drifting mustn't launch the car)
		if (control && driver && g_mcMove && (g_flyAllowed || g_carFly.on) && natives::IsDisabledControlJustPressed(0, 76) && (g_carFly.on || natives::GetEntitySpeed(car) < 12.0f))
		{
			toggle = now - g_carFly.lastSpace < 350;
			g_carFly.lastSpace = toggle ? -100000 : now;
		}
		if (g_carFly.on && (toggle || !control || !driver || car != g_carFly.car || !g_mcMove || !g_flyAllowed ||
			(g_walk.acked && g_walk.ground && now - g_carFly.startedAt > 1200)))
		{
			car_fly_end();
			return 0;
		}
		if (!g_carFly.on)
		{
			if (!toggle)
				return 0;
			// take off: the car's centre from the player's feet, in the car's frame, kept all the way
			const Vector3 p = natives::GetEntityCoords(ped, TRUE);
			const Vector3 feet = natives::GetOffsetFromEntityGivenWorldCoords(car, p.x, p.y, p.z - 1.0f);
			g_carFly = {true, car, -feet.x, -feet.y, -feet.z, -100000, now, g_carFly.teleportAt};
			natives::FreezeEntityPosition(car, TRUE);
			g_walk.x = p.x;
			g_walk.y = p.y;
			g_walk.z = g_walk.standZ = p.z - 1.0f + 0.3f; // (a little up, off the ground)
			g_walk.acked = false;
			g_drive.havePos = false;
			g_drive.vel = {};
			g_proxy.floor.clear();
			g_proxy.walls.clear();
			g_proxy.ceilings.clear();
			g_proxy.floorBase = -100000.0f;
			proxy_tick(ped, true);
			walk_correct(false, "car flight");
			natives::Notify("The car flies: ~y~W A S D~s~, ~y~Space~s~ up, ~y~Ctrl~s~ down, ~y~Shift~s~ faster. ~y~Space~s~ twice to drive");
		}
		const int in = car_fly_input();
		if (g_walk.acked && g_drive.havePos)
		{
			const float dt = std::clamp(float(now_nanos() - g_drive.posNanos) * 1e-9f, 0.0f, 0.05f);
			const float tx = g_drive.pos.x + g_drive.vel.x * dt, ty = g_drive.pos.y + g_drive.vel.y * dt, tz = g_drive.pos.z + g_drive.vel.z * dt;
			const float jx = tx - g_walk.x, jy = ty - g_walk.y, jz = tz - g_walk.z, jump = std::sqrt(jx * jx + jy * jy + jz * jz);
			if (now - g_carFly.teleportAt < 1500 || jump > 30.0f)
			{
				// an ender pearl moved the car: Minecraft's player goes where it is now
				const Vector3 p = natives::GetEntityCoords(ped, TRUE);
				g_walk.x = p.x;
				g_walk.y = p.y;
				g_walk.z = p.z - 1.0f + 0.3f;
				if (now - g_carFly.teleportAt < 100)
					walk_correct(false, "car pearl");
			}
			else
			{
				g_walk.x = tx;
				g_walk.y = ty;
				g_walk.z = tz;
			}
		}
		const float heading = natives::GetGameplayCamRot(2).z, h = heading * 3.14159265f / 180.0f;
		const float cs = std::cos(h), sn = std::sin(h);
		const float cx = g_walk.x + g_carFly.ox * cs - g_carFly.oy * sn, cy = g_walk.y + g_carFly.ox * sn + g_carFly.oy * cs;
		natives::SetEntityCoordsNoOffset(car, cx, cy, g_walk.z + g_carFly.oz);
		natives::SetEntityRotation(car, 0.0f, 0.0f, heading);
		proxy_tick(ped, false);
		return in;
	}

	/// A Minecraft animal GTA has too ({"t":"animal","m":"a_c_cow","type":28,"pos":[x,y,z],"yaw":deg}, Minecraft
	/// coordinates): GTA's own takes its place (Minecraft has removed its mob).
	void animal_message(const std::string &m)
	{
		const std::string model = json_str(m, "m");
		const char *pos = json_value(m, "pos");
		double x, y, z;
		if (model.empty() || pos == nullptr || sscanf_s(pos, "[%lf ,%lf ,%lf ]", &x, &y, &z) != 3 || g_animalQueue.size() > 64)
			return;
		const Hash hash = natives::GetHashKey(model.c_str());
		if (!natives::IsModelValid(hash))
			return;
		natives::RequestModel(hash);
		g_animalQueue.push_back({hash, int(json_num(m, "type", 28.0)), float(x), float(-z), float(y - g_yOffset),
			wrap_degrees(180.0f - float(json_num(m, "yaw", 0.0))), natives::GetGameTimer() + 5000});
	}

	/// Spawn the queued animals whose models are in; GTA looks after them from then on (they wander off, flee, die).
	void animals_tick()
	{
		const int now = natives::GetGameTimer();
		for (auto it = g_animalQueue.begin(); it != g_animalQueue.end();)
		{
			if (natives::HasModelLoaded(it->model))
			{
				Ped a = natives::CreatePed(it->pedType, it->model, it->x, it->y, it->z, it->heading);
				if (a != 0)
				{
					natives::TaskWanderStandard(a);
					natives::SetPedAsNoLongerNeeded(&a);
				}
				natives::SetModelAsNoLongerNeeded(it->model);
				it = g_animalQueue.erase(it);
			}
			else if (now > it->until)
				it = g_animalQueue.erase(it);
			else
				++it;
		}
	}

	/// Minecraft's mobs near the player ({"t":"allmobs","m":[[id,x,y,z,width,height,onFire],...]}, Minecraft
	/// coordinates), for GTA's world to act on (world_mobs_tick).
	void world_mobs_message(const std::string &m)
	{
		g_worldMobs.clear();
		const char *at = json_value(m, "m");
		while (at && (at = std::strchr(at + 1, '[')) != nullptr)
		{
			int id = 0, fire = 0;
			double x, y, z, w, h;
			if (sscanf_s(at, "[%d,%lf,%lf,%lf,%lf,%lf,%d]", &id, &x, &y, &z, &w, &h, &fire) == 7)
				g_worldMobs.push_back({id, float(x), float(-z), float(y - g_yOffset), float(w), float(h), fire != 0});
		}
	}

	/// Ten times a second, GTA's world on Minecraft's mobs: a car driving into one, or a person flung into one, kills
	/// it; burning people and cars set it alight, and a burning mob sets people alight.
	void world_mobs_tick(Ped player)
	{
		const int now = natives::GetGameTimer();
		if (g_worldMobs.empty() || now < g_nextWorldMobsAt || g_noInput)
			return;
		g_nextWorldMobsAt = now + 100;
		const Vector3 me = natives::GetEntityCoords(player, TRUE);
		std::set<int> kill, burn;
		auto touching = [](const WorldMob &mob, const Vector3 &o, float r) {
			const float dx = o.x - mob.x, dy = o.y - mob.y;
			return dx * dx + dy * dy < (r + mob.width * 0.5f) * (r + mob.width * 0.5f) && o.z > mob.z - 1.2f && o.z < mob.z + mob.height + 1.0f;
		};
		each_vehicle_near(me.x, me.y, me.z, 80.0f, [&](Vehicle v, float, float, float) {
			const Vector3 vel = natives::GetEntityVelocity(v);
			const bool moving = vel.x * vel.x + vel.y * vel.y + vel.z * vel.z > 4.0f * 4.0f, burning = natives::IsEntityOnFire(v);
			if (!moving && !burning)
				return;
			Vector3 mn = {}, mx = {};
			natives::GetModelDimensions(natives::GetEntityModel(v), &mn, &mx);
			const Vector3 c = natives::GetEntityCoords(v, TRUE);
			for (const WorldMob &mob : g_worldMobs)
			{
				if ((mob.x - c.x) * (mob.x - c.x) + (mob.y - c.y) * (mob.y - c.y) > 8.0f * 8.0f)
					continue;
				const Vector3 l = natives::GetOffsetFromEntityGivenWorldCoords(v, mob.x, mob.y, mob.z + mob.height * 0.5f);
				const float pad = 0.3f + mob.width * 0.5f;
				if (l.x < mn.x - pad || l.x > mx.x + pad || l.y < mn.y - pad || l.y > mx.y + pad || l.z < mn.z - mob.height || l.z > mx.z + 0.5f)
					continue;
				if (moving)
					kill.insert(mob.id); // run over
				else if (burning)
					burn.insert(mob.id);
			}
		});
		int handles[256];
		const int n = worldGetAllPeds(handles, 256);
		for (int i = 0; i < n; ++i)
		{
			const Ped q = handles[i];
			if (q == player || g_doublePeds.count(q))
				continue;
			const Vector3 o = natives::GetEntityCoords(q, TRUE);
			if ((o.x - me.x) * (o.x - me.x) + (o.y - me.y) * (o.y - me.y) > 80.0f * 80.0f)
				continue;
			const Vector3 vel = natives::GetEntityVelocity(q);
			const bool flung = natives::IsPedRagdoll(q) && vel.x * vel.x + vel.y * vel.y + vel.z * vel.z > 3.5f * 3.5f;
			const bool burning = natives::IsEntityOnFire(q);
			for (const WorldMob &mob : g_worldMobs)
			{
				if (!touching(mob, o, 0.6f))
					continue;
				if (flung)
					kill.insert(mob.id);
				if (burning)
					burn.insert(mob.id);
				if (mob.fire && !burning && !natives::IsPedDeadOrDying(q))
					natives::StartEntityFire(q);
			}
		}
		auto send = [](const char *type, const std::set<int> &ids) {
			if (ids.empty())
				return;
			std::string out = std::string("{\"t\":\"") + type + "\",\"ids\":[";
			for (const int id : ids)
				out += (out.back() == '[' ? "" : ",") + std::to_string(id);
			g_ws.send(out + "]}");
		};
		send("mobkill", kill);
		send("mobfire", burn);
	}

	/// Twice a second: GTA's fires near the player (a molotov's, a burning car's, a gas line's) go to Minecraft, which
	/// lights fire blocks there and sets its mobs in them alight.
	void fires_tick(Ped player)
	{
		const int now = natives::GetGameTimer();
		if (now < g_nextFiresAt || g_noInput)
			return;
		g_nextFiresAt = now + 500;
		const Vector3 me = natives::GetEntityCoords(player, TRUE);
		if (natives::GetNumberOfFiresInRange(me.x, me.y, me.z, 40.0f) == 0)
		{
			g_firesSent.clear();
			return;
		}
		std::set<std::tuple<int, int, int>> fires;
		for (int i = -1; i <= 1; ++i)
			for (int j = -1; j <= 1; ++j)
			{
				Vector3 f = {};
				if (natives::GetClosestFirePos(&f, me.x + i * 14.0f, me.y + j * 14.0f, me.z) &&
					(f.x - me.x) * (f.x - me.x) + (f.y - me.y) * (f.y - me.y) < 45.0f * 45.0f)
					fires.emplace(int(std::floor(f.x)), int(std::floor(f.z + g_yOffset)), int(std::floor(-f.y)));
			}
		std::string list;
		for (const auto &[x, y, z] : fires)
			list += (list.empty() ? "" : ",") + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z);
		if (list == g_firesSent)
			return;
		g_firesSent = list;
		g_ws.send("{\"t\":\"gtafire\",\"p\":[" + list + "]}");
	}

	/// A Minecraft command that reaches GTA's world ({"t":"gtacmd","c":...}): /kill @e (GTA's people and animals too),
	/// /time, /weather, /clearall.

	/// The amplifier (0 = level I) of a Minecraft effect on its player, or -1.
	int mc_effect(const char *name)
	{
		const std::string key = std::string(name) + ":";
		const size_t at = g_mcEffects.find(key);
		if (at == std::string::npos || (at > 0 && g_mcEffects[at - 1] != ','))
			return -1;
		return std::atoi(g_mcEffects.c_str() + at + key.size());
	}

	void cheats_tick(Ped ped)
	{
		const Player player = natives::PlayerId();
		potion_tick();
		// Minecraft's effects in GTA's movement too: speed and slowness, jump boost, strength, fire resistance, nausea
		{
			static std::string applied = "-";
			const int speed = mc_effect("speed"), slow = mc_effect("slowness"), jump = mc_effect("jump_boost");
			if (jump >= 1)
				natives::SetSuperJumpThisFrame(player);
			if (slow >= 0)
				natives::SetPedMoveRateOverride(ped, std::max(0.3f, 1.0f - 0.15f * float(slow + 1)));
			if (applied != g_mcEffects)
			{
				applied = g_mcEffects;
				natives::SetRunSprintMultiplierForPlayer(player, g_cheats.fastRun ? 1.49f : speed >= 0 ? std::min(1.49f, 1.0f + 0.12f * float(speed + 1)) : 1.0f);
				const int strong = mc_effect("strength");
				natives::SetPlayerMeleeWeaponDamageModifier(player, strong >= 0 ? 1.0f + 0.8f * float(strong + 1) : 1.0f);
				natives::SetPedIsDrunk(ped, g_cheats.drunk || mc_effect("nausea") >= 0);
				// night vision as GTA's own goggles; invisible, GTA's people (and the police) don't see the player
				natives::SetNightvision(mc_effect("night_vision") >= 0);
				if (!g_mcSpectator)
					natives::SetEveryoneIgnorePlayer(player, mc_effect("invisibility") >= 0);
			}
		}
		if (g_cheats.superJump)
			natives::SetSuperJumpThisFrame(player);
		if (g_cheats.explosiveAmmo)
			natives::SetExplosiveAmmoThisFrame(player);
		if (g_cheats.fireAmmo)
			natives::SetFireAmmoThisFrame(player);
		if (g_cheats.explosiveMelee)
			natives::SetExplosiveMeleeThisFrame(player);
		if (g_cheats.neverWanted)
			natives::ClearPlayerWantedLevel(player);
		if (g_cheats.slidey && natives::IsPedInAnyVehicle(ped, FALSE))
			natives::SetVehicleReduceGrip(natives::GetVehiclePedIsIn(ped, FALSE), TRUE);
		if (g_cheats.traffic >= 0.0f)
		{
			natives::SetVehicleDensityMultiplierThisFrame(g_cheats.traffic);
			natives::SetRandomVehicleDensityMultiplierThisFrame(g_cheats.traffic);
			natives::SetParkedVehicleDensityMultiplierThisFrame(g_cheats.traffic);
		}
		if (g_cheats.crowds >= 0.0f)
		{
			natives::SetPedDensityMultiplierThisFrame(g_cheats.crowds);
			natives::SetScenarioPedDensityMultiplierThisFrame(g_cheats.crowds, g_cheats.crowds);
		}
		// a totem of undying in hand: about to die, GTA's player is saved instead (and the totem's used up, as in Minecraft)
		if (g_totemBuffer != 0 && (g_totemPed != ped || !g_mcTotem || g_godMode || natives::IsEntityDead(ped)))
			totem_unbuffer();
		// the totem's hidden health left on from before (a save made while it was held, the plugin reloaded): GTA's
		// own health again (the story characters' 200), or the player took thousands of damage to die
		if (g_totemBuffer == 0 && natives::GetEntityMaxHealth(ped) > 1000 && !natives::IsEntityDead(ped))
		{
			logf("health: max %d left over from a totem: back to 200", natives::GetEntityMaxHealth(ped));
			natives::SetEntityMaxHealth(ped, 200);
			if (natives::GetEntityHealth(ped) > 200)
				natives::SetEntityHealth(ped, 200);
		}
		if (g_mcTotem && !g_godMode && !natives::IsEntityDead(ped))
		{
			if (g_totemBuffer == 0)
			{
				// (one hit from full health to dead never went past "low" for the totem to act on)
				natives::SetEntityMaxHealth(ped, natives::GetEntityMaxHealth(ped) + kTotemBuffer);
				natives::SetEntityHealth(ped, natives::GetEntityHealth(ped) + kTotemBuffer);
				g_totemBuffer = kTotemBuffer;
				g_totemPed = ped;
			}
			const int max = real_max_health(ped) - 100, hp = real_health(ped) - 100;
			if (max > 0 && hp < max / 8)
			{
				totem_unbuffer();
				natives::SetEntityHealth(ped, 100 + max / 2);
				natives::StopEntityFire(ped);
				g_mcTotem = false; // (until Minecraft says it holds another)
				g_ws.send("{\"t\":\"totem\"}");
				shake_impulse(0.3f);
			}
		}
		// golden apples' absorption: GTA body armour on top
		{
			static int absorbed = -1;
			const int absorption = mc_effect("absorption");
			if (absorption >= 0 && absorbed < 0)
				natives::SetPedArmour(ped, std::min(100, natives::GetPedArmour(ped) + 20 * (absorption + 1)));
			absorbed = absorption;
		}
		// compasses point to the waypoint on GTA's map (Minecraft's compasses point at its world spawn)
		{
			static int nextCompass = 0;
			static float wx = 1e9f, wy = 1e9f;
			const int now = natives::GetGameTimer();
			if (now >= nextCompass)
			{
				nextCompass = now + 2000;
				if (natives::IsWaypointActive())
				{
					const Vector3 w = natives::GetBlipInfoIdCoord(natives::GetFirstBlipInfoId(8));
					if (std::fabs(w.x - wx) + std::fabs(w.y - wy) > 2.0f)
					{
						wx = w.x;
						wy = w.y;
						// (compasses point there: not the world's spawn, which moved the player's respawn point too)
						sendf("{\"t\":\"waypoint\",\"c\":[%d,%d,%d]}", int(std::floor(w.x)), 64, int(std::floor(-w.y)));
					}
				}
			}
		}
		// GTA's guns at Minecraft's blocks: each shot damages the block it hits (Minecraft breaks it once that adds up, by
		// how hard it is); rockets and grenades blow Minecraft's blocks up around where they land
		if (natives::IsPedShooting(ped))
		{
			Vector3 at = {};
			static Vector3 lastShot = {};
			if (natives::GetPedLastWeaponImpactCoord(ped, &at) && (at.x != 0.0f || at.y != 0.0f) &&
				(at.x != lastShot.x || at.y != lastShot.y || at.z != lastShot.z))
			{
				lastShot = at;
				const Vector3 c = natives::GetFinalRenderedCamCoord();
				const float dx = at.x - c.x, dy = at.y - c.y, dz = at.z - c.z, dl = std::max(0.01f, std::sqrt(dx * dx + dy * dy + dz * dz));
				const Hash weapon = natives::GetSelectedPedWeapon(ped);
				const bool explosive = natives::GetWeaponDamageType(weapon) == 5 || g_cheats.explosiveAmmo;
				// only near Minecraft's blocks (props): GTA's own walls aren't Minecraft's
				const float ix = at.x + dx / dl * 0.15f, iy = at.y + dy / dl * 0.15f, iz = at.z + dz / dl * 0.15f;
				bool nearBlock = false;
				const int bx = int(std::floor(ix)), bz = int(std::floor(-iy)), by = int(std::floor(iz + g_yOffset));
				const int r = explosive ? 3 : 1;
				for (int ox = -r; ox <= r && !nearBlock; ++ox)
					for (int oy = -r; oy <= r && !nearBlock; ++oy)
						for (int oz = -r; oz <= r && !nearBlock; ++oz)
							nearBlock = g_props.blocks.count({bx + ox, by + oy, bz + oz}) != 0;
				if (nearBlock)
					sendf("{\"t\":\"hostshot\",\"pos\":[%.3f,%.3f,%.3f],\"dir\":[%.3f,%.3f,%.3f],\"boom\":%s}", at.x, at.z + g_yOffset, -at.y,
						dx / dl, dz / dl, -dy / dl, explosive ? "true" : "false");
			}
		}
		// a shield held up: bullets and blows don't get through (GTA's damage, as Minecraft's own is blocked by Minecraft);
		// fire resistance: fire doesn't. All in one place, applied again whenever either changes, the player is another
		// character, or F9 set GTA's own rules (each had its own, and each undid the other's)
		{
			static Ped provedPed = 0;
			static int proved = -1;
			const bool fireproof = mc_effect("fire_resistance") >= 0;
			const int want = (g_mcBlocking ? 1 : 0) | (fireproof ? 2 : 0);
			if (g_godMode)
				proved = -1; // (god mode's own proofs: make_safe)
			else if (want != proved || ped != provedPed || g_proofsDirty)
			{
				proved = want;
				provedPed = ped;
				g_proofsDirty = false;
				natives::SetEntityProofs(ped, g_mcBlocking, fireproof, FALSE, FALSE, g_mcBlocking);
			}
		}
	}

	/// /gta <setting> [on|off|value]: GTA's cheats and abilities (superjump, fastrun, fastswim, explosiveammo, fireammo,
	/// explosivemelee, slidey, moon, slowmo, infiniteammo, neverwanted, drunk, onehit, weapons, heal, armor, wanted N),
	/// and spawning: /gta spawn car|truck|tank|plane|heli|boat|bike|npc|cop|<model>.
	// A teleport onto the ground somewhere GTA hasn't loaded yet (/waypoint, /gta tp x y): the player waits high above
	// it, a frame at a time, till GTA has its ground there (or 4 s), then is put on it
	struct Landing
	{
		bool on = false;
		float x = 0, y = 0, fallbackZ = 40.0f;
		int until = 0;
		bool scene = false;
		std::string note;
	} g_landing;

	void land_start(Ped player, float x, float y, float fallbackZ, bool loadScene, const char *note)
	{
		if (g_drive.on)
			drive_set(player, false);
		if (g_carFly.on)
			car_fly_end();
		if (g_landing.on && g_landing.scene)
			natives::NewLoadSceneStop();
		g_landing = {true, x, y, fallbackZ, natives::GetGameTimer() + 4000, loadScene, note != nullptr ? note : ""};
		if (loadScene)
			natives::NewLoadSceneStartSphere(x, y, 100.0f, 120.0f);
	}

	void landing_tick(Ped player)
	{
		if (!g_landing.on)
			return;
		const bool inCar = natives::IsPedInAnyVehicle(player, FALSE) != FALSE;
		const Entity mover = inCar ? Entity(natives::GetVehiclePedIsIn(player, FALSE)) : Entity(player);
		float gz = 0.0f;
		bool found = false;
		for (const float z : {1000.0f, 700.0f, 400.0f, 200.0f, 100.0f, 50.0f, 20.0f})
			if (natives::GetGroundZFor3dCoord(g_landing.x, g_landing.y, z, &gz, FALSE, FALSE))
			{
				found = true;
				break;
			}
		if (!found && natives::GetGameTimer() < g_landing.until)
		{
			natives::SetEntityCoordsNoOffset(mover, g_landing.x, g_landing.y, 900.0f);
			natives::RequestCollisionAtCoord(g_landing.x, g_landing.y, 100.0f);
			return;
		}
		g_landing.on = false;
		if (g_landing.scene)
			natives::NewLoadSceneStop();
		if (!found)
			gz = g_landing.fallbackZ;
		natives::SetEntityCoordsNoOffset(mover, g_landing.x, g_landing.y, gz + (inCar ? 0.6f : 1.0f));
		natives::SetGameplayCamRelativeHeading(0.0f);
		if (g_walk.on)
		{
			g_walk.x = g_landing.x;
			g_walk.y = g_landing.y;
			g_walk.z = g_walk.standZ = gz;
			g_walk.teleportAt = natives::GetGameTimer();
			g_walk.haveEst = false;
			walk_correct(false, "teleport");
		}
		g_haveOffset = false; // Minecraft's ground starts over here (and its player is put there)
		if (!g_landing.note.empty())
			natives::Notify(g_landing.note.c_str());
	}

	void gta_command(Ped player, const std::string &args)
	{
		std::string what = args, value;
		for (char &ch : what)
			ch = char(std::tolower(static_cast<unsigned char>(ch)));
		if (const size_t sp = what.find(' '); sp != std::string::npos)
		{
			value = what.substr(sp + 1);
			what.resize(sp);
		}
		const Player me = natives::PlayerId();
		auto toggle = [&](bool &flag, const char *name) {
			flag = value.empty() ? !flag : !(value == "off" || value == "0" || value == "false");
			natives::Notify((std::string(name) + (flag ? " ~g~on" : " ~r~off")).c_str());
			return flag;
		};
		if (what == "spawn" || what == "summon")
			summon(player, value);
		else if (what == "superjump" || what == "jump")
		{
			const bool on = toggle(g_cheats.superJump, "Super jump");
			// (Minecraft's movement jumps by Minecraft: its jump boost)
			sendf("{\"t\":\"cmd\",\"c\":\"%s\"}", on ? "effect give @a minecraft:jump_boost infinite 4 true" : "effect clear @a minecraft:jump_boost");
		}
		else if (what == "fastrun" || what == "superspeed" || what == "speed")
		{
			const bool on = toggle(g_cheats.fastRun, "Super speed");
			natives::SetRunSprintMultiplierForPlayer(me, on ? 1.49f : 1.0f);
			sendf("{\"t\":\"cmd\",\"c\":\"%s\"}", on ? "effect give @a minecraft:speed infinite 3 true" : "effect clear @a minecraft:speed");
		}
		else if (what == "fastswim")
			natives::SetSwimMultiplierForPlayer(me, toggle(g_cheats.fastSwim, "Fast swim") ? 1.49f : 1.0f);
		else if (what == "explosiveammo" || what == "explosivebullets")
			toggle(g_cheats.explosiveAmmo, "Explosive bullets");
		else if (what == "fireammo" || what == "firebullets")
			toggle(g_cheats.fireAmmo, "Fire bullets");
		else if (what == "explosivemelee" || what == "explosivepunch")
			toggle(g_cheats.explosiveMelee, "Explosive melee");
		else if (what == "slidey" || what == "slideycars")
		{
			if (!toggle(g_cheats.slidey, "Slidey cars") && natives::IsPedInAnyVehicle(player, FALSE))
				natives::SetVehicleReduceGrip(natives::GetVehiclePedIsIn(player, FALSE), FALSE);
		}
		else if (what == "moon" || what == "lowgravity" || what == "gravity")
			natives::SetGravityLevel(toggle(g_cheats.moon, "Moon gravity") ? 2 : 0);
		else if (what == "slowmo")
			natives::SetTimeScale(toggle(g_cheats.slowmo, "Slow motion") ? 0.4f : 1.0f);
		else if (what == "infiniteammo")
			natives::SetPedInfiniteAmmoClip(player, toggle(g_cheats.infiniteAmmo, "Infinite ammo"));
		else if (what == "neverwanted")
			toggle(g_cheats.neverWanted, "Never wanted");
		else if (what == "drunk")
			natives::SetPedIsDrunk(player, toggle(g_cheats.drunk, "Drunk"));
		else if (what == "onehit" || what == "oneshot")
			natives::SetPlayerWeaponDamageModifier(me, toggle(g_cheats.oneHit, "One-hit kills") ? 100.0f : 1.0f);
		else if (what == "wanted")
		{
			const int level = std::clamp(std::atoi(value.c_str()), 0, 5);
			if (level == 0)
				natives::ClearPlayerWantedLevel(me);
			else
			{
				natives::SetMaxWantedLevel(5);
				natives::SetPlayerWantedLevel(me, level);
				natives::SetPlayerWantedLevelNow(me);
			}
		}
		else if (what == "traffic" || what == "crowds" || what == "peds")
		{
			float &m = what == "traffic" ? g_cheats.traffic : g_cheats.crowds;
			m = value.empty() || value == "normal" ? -1.0f : value == "none" || value == "off" ? 0.0f : std::clamp(float(std::atof(value.c_str())), 0.0f, 3.0f);
			char note[96];
			snprintf(note, sizeof(note), "%s: %s", what == "traffic" ? "Traffic" : "Crowds", m < 0.0f ? "GTA's own" : (std::to_string(m).substr(0, 4) + "x").c_str());
			natives::Notify(note);
		}
		else if (what == "blackout")
			natives::SetArtificialLightsState(toggle(g_cheats.blackout, "Blackout"));
		else if (what == "freezetime" || what == "pausetime")
			natives::PauseClock(toggle(g_cheats.frozenTime, "Time frozen"));
		else if (what == "clear")
		{
			const Vector3 at = natives::GetEntityCoords(player, TRUE);
			natives::ClearAreaOfPeds(at.x, at.y, at.z, 80.0f);
			natives::ClearAreaOfVehicles(at.x, at.y, at.z, 80.0f);
			natives::Notify("Cleared GTA's people and cars around");
		}
		else if (what == "flip" || what == "fix" || what == "boost")
		{
			if (!natives::IsPedInAnyVehicle(player, FALSE))
			{
				natives::Notify("Get in a car first");
				return;
			}
			const Vehicle v = natives::GetVehiclePedIsIn(player, FALSE);
			if (what == "flip")
			{
				const Vector3 rot = natives::GetEntityRotation(v);
				natives::SetEntityRotation(v, 0.0f, 0.0f, rot.z);
				natives::SetVehicleOnGroundProperly(v);
			}
			else if (what == "fix")
			{
				natives::SetVehicleFixed(v);
				natives::SetVehicleDirtLevel(v, 0.0f);
			}
			else
				natives::SetVehicleForwardSpeed(v, std::max(natives::GetEntitySpeed(v), 0.0f) + (value.empty() ? 30.0f : float(std::atof(value.c_str()))));
		}
		else if (what == "sethome")
		{
			g_cheats.home = natives::GetEntityCoords(player, TRUE);
			g_cheats.haveHome = true;
			natives::Notify("Home set here (~y~/gta home~s~ to come back)");
		}
		else if (what == "home" || what == "tp" || what == "skyfall")
		{
			Vector3 to = natives::GetEntityCoords(player, TRUE);
			if (what == "home")
			{
				if (!g_cheats.haveHome)
				{
					natives::Notify("No home yet: ~y~/gta sethome~s~ first");
					return;
				}
				to = g_cheats.home;
			}
			else if (what == "tp")
			{
				float x = 0, y = 0, z = 0;
				if (sscanf_s(value.c_str(), "%f %f %f", &x, &y, &z) < 2)
				{
					natives::Notify("~y~/gta tp~s~ x y [z] (GTA's map coordinates)");
					return;
				}
				to = Vector3{x, 0, y, 0, z != 0.0f ? z : 1000.0f, 0};
			}
			else
				to.z += 800.0f;
			const bool inCar = natives::IsPedInAnyVehicle(player, FALSE) != FALSE;
			const Entity mover = inCar ? Entity(natives::GetVehiclePedIsIn(player, FALSE)) : Entity(player);
			natives::RequestCollisionAtCoord(to.x, to.y, to.z);
			if (what == "tp" && to.z == 1000.0f)
			{
				// no height given: onto the ground there (once GTA has loaded it)
				land_start(player, to.x, to.y, 40.0f, false, nullptr);
				return;
			}
			natives::SetEntityCoordsNoOffset(mover, to.x, to.y, to.z);
			if (g_walk.on)
			{
				g_walk.x = to.x;
				g_walk.y = to.y;
				g_walk.z = g_walk.standZ = to.z - 1.0f;
				g_walk.teleportAt = natives::GetGameTimer();
				g_walk.haveEst = false;
				walk_correct(false, "pearl");
			}
			g_haveOffset = false;
		}
		else if (what == "ragdoll")
			natives::SetPedToRagdoll(player, 3000);
		else if (what == "heal")
			natives::SetEntityHealth(player, natives::GetEntityMaxHealth(player));
		else if (what == "armor" || what == "armour")
			natives::SetPedArmour(player, 100);
		else if (what == "weapons")
		{
			for (const char *w : {"weapon_pistol", "weapon_combatpistol", "weapon_smg", "weapon_assaultrifle", "weapon_carbinerifle", "weapon_pumpshotgun",
					 "weapon_sniperrifle", "weapon_heavysniper", "weapon_rpg", "weapon_grenadelauncher", "weapon_minigun", "weapon_grenade",
					 "weapon_stickybomb", "weapon_molotov", "weapon_knife", "weapon_bat", "weapon_railgun", "weapon_firework"})
				natives::GiveWeaponToPed(player, natives::GetHashKey(w), 9999, FALSE, FALSE);
			natives::Notify("All ~y~weapons~s~ (Tab for GTA weapons)");
		}
		else
		{
			natives::Notify("~y~/gta~s~ spawn <car|truck|tank|plane|heli|boat|bike|npc|cop|animal|model>, superjump, fastrun, fastswim, explosiveammo, "
							"fireammo, explosivemelee, slidey, moon, slowmo, infiniteammo, neverwanted, onehit, drunk, wanted 0-5, heal, armor, weapons");
			natives::Notify("~y~/gta~s~ traffic 0-3, crowds 0-3, blackout, freezetime, clear, flip, fix, boost, sethome, home, tp x y [z], skyfall, ragdoll");
		}
	}

	/// /tune for the car the player is in: "max" (every performance part at its best, turbo on), "stock", a part and a
	/// level ("engine 2", "brakes max", "turbo off", "wheels 12"), "visuals" (a random body kit), "repair".
	void tune(Ped player, const std::string &args)
	{
		if (!natives::IsPedInAnyVehicle(player, FALSE))
		{
			natives::Notify("Get in a car to ~y~/tune~s~ it");
			return;
		}
		const Vehicle v = natives::GetVehiclePedIsIn(player, FALSE);
		natives::SetVehicleModKit(v, 0);
		std::string what = args, level;
		for (char &ch : what)
			ch = char(std::tolower(static_cast<unsigned char>(ch)));
		if (const size_t sp = what.find(' '); sp != std::string::npos)
		{
			level = what.substr(sp + 1);
			what.resize(sp);
		}
		if (what.empty())
			what = "max";
		auto set = [&](int type, const std::string &lv) {
			const int n = natives::GetNumVehicleMods(v, type);
			if (n <= 0)
				return;
			if (lv == "stock" || lv == "off")
			{
				natives::RemoveVehicleMod(v, type);
				return;
			}
			int i = n - 1;
			if (!lv.empty() && lv != "max")
				i = std::clamp(std::atoi(lv.c_str()) - 1, -1, n - 1); // 1 = the first upgrade, 0 = stock
			if (i < 0)
				natives::RemoveVehicleMod(v, type);
			else
				natives::SetVehicleMod(v, type, i, FALSE);
		};
		static const std::map<std::string, std::vector<int>> parts = {
			{"engine", {11}}, {"brakes", {12}}, {"transmission", {13}}, {"gearbox", {13}}, {"suspension", {15}}, {"armor", {16}}, {"armour", {16}},
			{"spoiler", {0}}, {"bumpers", {1, 2}}, {"skirts", {3}}, {"exhaust", {4}}, {"grille", {6}}, {"hood", {7}}, {"roof", {10}},
			{"horn", {14}}, {"wheels", {23}}, {"livery", {48}},
		};
		const int performance[] = {11, 12, 13, 15, 16};
		if (what == "max")
		{
			for (const int t : performance)
				set(t, "max");
			natives::ToggleVehicleMod(v, 18, TRUE);
			natives::Notify("Tuned to the ~g~max~s~: engine, brakes, transmission, suspension, armour, turbo");
		}
		else if (what == "stock")
		{
			for (int t = 0; t < 50; ++t)
				natives::RemoveVehicleMod(v, t);
			natives::ToggleVehicleMod(v, 18, FALSE);
			natives::ToggleVehicleMod(v, 22, FALSE);
			natives::SetVehicleWindowTint(v, 0);
			natives::Notify("Back to ~y~stock");
		}
		else if (what == "turbo" || what == "xenon")
		{
			natives::ToggleVehicleMod(v, what == "turbo" ? 18 : 22, level != "off" && level != "0");
			natives::Notify((what + (level != "off" && level != "0" ? " ~g~on" : " ~r~off")).c_str());
		}
		else if (what == "tint")
		{
			natives::SetVehicleWindowTint(v, level.empty() ? 1 : std::clamp(std::atoi(level.c_str()), 0, 6));
		}
		else if (what == "visuals")
		{
			for (const int t : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10})
			{
				const int n = natives::GetNumVehicleMods(v, t);
				if (n > 0)
					natives::SetVehicleMod(v, t, natives::GetGameTimer() / (t + 3) % n, FALSE);
			}
			natives::Notify("A new ~y~look");
		}
		else if (what == "repair")
		{
			natives::SetVehicleFixed(v);
			natives::SetVehicleDirtLevel(v, 0.0f);
		}
		else if (const auto it = parts.find(what); it != parts.end())
		{
			for (const int t : it->second)
				set(t, level.empty() ? "max" : level);
			char note[96];
			snprintf(note, sizeof(note), "%s: %s (%d levels)", what.c_str(), level.empty() ? "max" : level.c_str(),
				natives::GetNumVehicleMods(v, it->second[0]));
			natives::Notify(note);
		}
		else
			natives::Notify("~y~/tune~s~ max, stock, engine 1-4, brakes, transmission, suspension, armor, turbo on/off, wheels N, visuals, xenon, tint 0-6, repair");
	}

	/// /summon with one of GTA's things: a kind (car, truck, tank, plane, heli, boat, bike, npc, cop) or any GTA
	/// vehicle or ped model name (adder, lazer, a_m_y_hipster_01): it appears in front of the player.
	void summon(Ped player, std::string what)
	{
		for (char &ch : what)
			ch = char(std::tolower(static_cast<unsigned char>(ch)));
		if (what.rfind("minecraft:", 0) == 0)
			what = what.substr(10);
		// "car 5": five of them; "car drive": and the player at its wheel
		int count = 1;
		bool drive = false;
		{
			std::string first, word;
			size_t at = 0;
			while (at < what.size())
			{
				const size_t sp = what.find(' ', at);
				word = what.substr(at, sp == std::string::npos ? std::string::npos : sp - at);
				at = sp == std::string::npos ? what.size() : sp + 1;
				if (word.empty())
					continue;
				if (first.empty())
					first = word;
				else if (word == "drive" || word == "in")
					drive = true;
				else if (std::atoi(word.c_str()) > 0)
					count = std::clamp(std::atoi(word.c_str()), 1, 20);
			}
			what = first;
		}
		static const std::map<std::string, std::vector<const char *>> kinds = {
			{"car", {"sultan", "buffalo", "adder", "elegy2", "dominator", "zentorno", "banshee", "oracle"}},
			{"truck", {"phantom", "mule", "benson", "packer", "hauler", "biff"}},
			{"tank", {"rhino"}},
			{"plane", {"lazer", "luxor", "velum", "duster", "shamal", "mammatus"}},
			{"jet", {"lazer", "hydra"}},
			{"heli", {"buzzard", "maverick", "frogger"}},
			{"helicopter", {"buzzard", "maverick", "frogger"}},
			{"boat", {"speeder", "jetmax", "seashark"}},
			{"bike", {"bati", "akuma", "sanchez", "hakuchou"}},
			{"motorcycle", {"bati", "akuma", "sanchez", "hakuchou"}},
			{"bus", {"bus", "coach"}},
			{"police", {"police", "police2", "police3"}},
			{"npc", {"a_m_y_hipster_01", "a_f_y_hipster_01", "a_m_m_business_01", "a_f_y_business_01", "a_m_y_skater_01", "a_m_m_tourist_01"}},
			{"ped", {"a_m_y_hipster_01", "a_f_y_hipster_01", "a_m_m_business_01", "a_f_y_business_01", "a_m_y_skater_01"}},
			{"person", {"a_m_y_hipster_01", "a_f_y_hipster_01", "a_m_m_business_01", "a_f_y_business_01"}},
			{"cop", {"s_m_y_cop_01", "s_f_y_cop_01"}},
			{"soldier", {"s_m_y_marine_01", "s_m_y_marine_03"}},
			{"animal", {"a_c_deer", "a_c_boar", "a_c_cow", "a_c_pig", "a_c_coyote", "a_c_mtlion", "a_c_husky", "a_c_retriever", "a_c_cat_01",
						   "a_c_rabbit_01", "a_c_hen", "a_c_chimp", "a_c_rhesus", "a_c_pigeon", "a_c_seagull", "a_c_crow"}},
		};
		std::string model = what;
		if (const auto it = kinds.find(what); it != kinds.end())
			model = it->second[size_t(natives::GetGameTimer()) % it->second.size()];
		const Hash h = natives::GetHashKey(model.c_str());
		if (model.empty() || !natives::IsModelInCdimage(h) || !natives::IsModelValid(h))
		{
			natives::Notify(("GTA has no ~r~" + what + "~s~ (try car, truck, tank, plane, heli, boat, bike, npc, cop, animal or a model name)").c_str());
			return;
		}
		natives::RequestModel(h);
		if (!natives::HasModelLoaded(h))
		{
			later([player, model] { summon(player, model); }); // (that model again, once loaded)
			return;
		}
		const bool vehicle = natives::IsModelAVehicle(h) != FALSE;
		const Vector3 cam = natives::GetFinalRenderedCamRot(2);
		const Vector3 me = natives::GetEntityCoords(player, TRUE);
		const float heading = cam.z, hr = heading * 3.14159265f / 180.0f;
		const float ahead = vehicle ? (what == "plane" || what == "jet" || model == "luxor" || model == "shamal" ? 18.0f : 7.0f) : 3.0f;
		const float rx = std::cos(hr), ry = std::sin(hr); // (to the right of where the camera looks)
		for (int i = 0; i < count; ++i)
		{
			// side by side, then further rows
			const float side = (float(i % 5) - float(std::min(count, 5) - 1) * 0.5f) * (vehicle ? 4.0f : 1.2f);
			const float back = float(i / 5) * (vehicle ? 7.0f : 1.5f);
			const float x = me.x - std::sin(hr) * (ahead + back) + rx * side, y = me.y + std::cos(hr) * (ahead + back) + ry * side;
			float gz = me.z;
			if (natives::GetGroundZFor3dCoord(x, y, me.z + 3.0f, &gz, FALSE, FALSE))
				gz += 0.5f;
			Entity e = 0;
			if (vehicle)
			{
				const Vehicle v = natives::CreateVehicle(h, x, y, gz + 0.5f, heading);
				natives::SetVehicleOnGroundProperly(v);
				if (drive && i == 0)
				{
					natives::SetPedIntoVehicle(player, v, -1);
					natives::SetVehicleEngineOn(v, TRUE);
				}
				e = v;
			}
			else
			{
				const Ped q = natives::CreatePed(ped_type(model), h, x, y, gz + 0.5f, heading + 180.0f);
				natives::TaskWanderStandard(q);
				e = q;
			}
			if (e != 0)
				natives::SetEntityAsNoLongerNeeded(&e);
		}
		natives::SetModelAsNoLongerNeeded(h);
	}

	// Endermen from the End steal GTA's cars: the nearest car is theirs, its driver thrown out, and it drives off with
	// the enderman in it (Minecraft keeps the enderman where the car is: "enderride").
	struct Thief
	{
		int mob;
		Vehicle car;
		Ped driver;
	};
	std::vector<Thief> g_thieves;

	// A bow drawn (or a loaded crossbow) aimed at GTA's people intimidates them: close up, hands up; further off, they run.
	int g_nextBowCheck = 0;
	std::unordered_map<Ped, int> g_intimidated; // until when

	// people a drawn bow made get out of their car: their hands go up once they're out (until when)
	std::unordered_map<Ped, int> g_carSurrender;

	/// Whether GTA person `q` fights rather than gives up at a drawn bow: the police and soldiers, anyone in a fight,
	/// anyone holding a gun.
	bool bow_defiant(Ped q, Ped player)
	{
		const int type = natives::GetPedType(q);
		return type == 6 || type == 27 || type == 29 || natives::IsPedInCombat(q, player) || natives::IsPedInCombat(q, 0) ||
			natives::IsPedArmed(q, 4 | 2);
	}

	/// Every 200 ms while Minecraft's bow is drawn (or a crossbow is loaded) in hand: the one person or car it points at
	/// reacts. A person puts their hands up (further off they run); a car's driver reacts as to a gun (see below). The
	/// police, soldiers and anyone fighting or armed only look back at the player: nobody surrenders mid gunfight.
	void bow_tick(Ped player)
	{
		const int now = natives::GetGameTimer();
		for (auto it = g_carSurrender.begin(); it != g_carSurrender.end();)
		{
			const Ped q = it->first;
			if (!natives::DoesEntityExist(q) || natives::IsPedDeadOrDying(q) || now > it->second)
				it = g_carSurrender.erase(it);
			else if (!natives::IsPedInAnyVehicle(q, FALSE))
			{
				natives::TaskHandsUp(q, 6000, player); // (out: hands up)
				natives::SetPedKeepTask(q, TRUE);
				g_intimidated[q] = now + 6000;
				it = g_carSurrender.erase(it);
			}
			else
				++it;
		}
		if (!g_bowDrawn || now - g_bowAt > 2500 || g_noInput || now < g_nextBowCheck)
			return;
		g_nextBowCheck = now + 200;
		const Vector3 c = natives::GetFinalRenderedCamCoord(), r = natives::GetFinalRenderedCamRot(2);
		const float d2r = 3.14159265f / 180.0f;
		const float fx = -std::sin(r.z * d2r) * std::cos(r.x * d2r), fy = std::cos(r.z * d2r) * std::cos(r.x * d2r), fz = std::sin(r.x * d2r);
		// what the bow points at: the first car or person on its line (a person is thin: the one nearest the line, close by it)
		Vector3 at = {}, n = {};
		Entity e = 0;
		Hash material = 0;
		const bool hit = world_probe(player, c.x, c.y, c.z, c.x + fx * 45.0f, c.y + fy * 45.0f, c.z + fz * 45.0f, 1 | 2 | 4 | 8 | 16, 7, at, n, e,
			material);
		const float limit = hit ? dist3(c, at) + 0.5f : 45.0f;
		Entity target = hit && e != 0 && (natives::GetEntityType(e) == 1 || natives::GetEntityType(e) == 2) ? e : 0;
		if (target == 0)
		{
			float best = 0.9f;
			each_ped_near(player, c.x, c.y, c.z, 45.0f, [&](Ped q, float, float, float) {
				const Vector3 o = natives::GetEntityCoords(q, TRUE);
				const float dx = o.x - c.x, dy = o.y - c.y, dz = o.z + 0.3f - c.z, d = std::sqrt(dx * dx + dy * dy + dz * dz);
				const float along = dx * fx + dy * fy + dz * fz;
				const float off = std::sqrt(std::max(0.0f, d * d - along * along));
				if (d >= 1.0f && along > 0.0f && along < limit && off < best)
				{
					best = off;
					target = q;
				}
			});
		}
		if (target == 0)
			return;
		if (natives::GetEntityType(target) == 1 && natives::IsPedInAnyVehicle(target, FALSE))
			target = natives::GetVehiclePedIsIn(target, FALSE);
		if (natives::GetEntityType(target) == 2)
		{
			// a car: as with a gun in GTA, its driver decides. Most get angry and drive on (honking), some back off and
			// flee, now and then one drives at the player, and a few give up: everyone gets out, hands up. (A police car's
			// people look back and stay in.)
			const Vehicle v = Vehicle(target);
			if (v == (natives::IsPedInAnyVehicle(player, FALSE) ? natives::GetVehiclePedIsIn(player, FALSE) : 0))
				return;
			const Ped driver = natives::GetPedInVehicleSeat(v, -1);
			if (driver != 0 && driver != player && !natives::IsPedDeadOrDying(driver) && !natives::IsEntityAMissionEntity(driver) &&
				!bow_defiant(driver, player))
			{
				const auto was = g_intimidated.find(driver);
				if (was != g_intimidated.end() && now < was->second)
					return;
				const int roll = natives::GetRandomIntInRange(0, 100);
				if (roll >= 15)
				{
					for (int seat = -1; seat < natives::GetVehicleMaxNumberOfPassengers(v); ++seat)
						if (const Ped q = natives::GetPedInVehicleSeat(v, seat); q != 0)
							g_intimidated[q] = now + 8000;
					if (roll < 40)
					{
						// backs off, then flees
						natives::TaskVehicleTempAction(driver, v, 3, 1500);
						after(now + 1500, [driver, player]() {
							if (natives::DoesEntityExist(driver) && !natives::IsPedDeadOrDying(driver))
								natives::TaskSmartFleePed(driver, player, 250.0f, 20000);
						});
					}
					else if (roll < 90)
					{
						// angry: honks and drives on, fast
						natives::StartVehicleHorn(v, 2500);
						natives::TaskVehicleDriveWander(driver, v, 40.0f, 1074528293 /* rushed */);
					}
					else
					{
						// drives at the player, then off
						natives::StartVehicleHorn(v, 1000);
						natives::TaskVehicleMissionPedTarget(driver, v, player, 2 /* ram */, 30.0f, 786469);
						after(now + 4500, [driver, v]() {
							if (natives::DoesEntityExist(driver) && natives::DoesEntityExist(v) && !natives::IsPedDeadOrDying(driver))
								natives::TaskVehicleDriveWander(driver, v, 30.0f, 786469);
						});
					}
					natives::SetPedKeepTask(driver, TRUE);
					return;
				}
			}
			for (int seat = -1; seat < natives::GetVehicleMaxNumberOfPassengers(v); ++seat)
			{
				const Ped q = natives::GetPedInVehicleSeat(v, seat);
				if (q == 0 || q == player || natives::IsPedDeadOrDying(q) || natives::IsEntityAMissionEntity(q) || g_carSurrender.count(q))
					continue;
				const auto was = g_intimidated.find(q);
				if (was != g_intimidated.end() && now < was->second)
					continue;
				if (bow_defiant(q, player))
				{
					natives::TaskLookAtEntity(q, player, 3000);
					g_intimidated[q] = now + 3000;
					continue;
				}
				natives::TaskLeaveVehicle(q, v, 256);
				g_carSurrender[q] = now + 6000;
				g_intimidated[q] = now + 6000;
			}
			return;
		}
		const Ped q = Ped(target);
		if (q == player || g_doublePeds.count(q) || natives::IsEntityAMissionEntity(q) || natives::IsPedDeadOrDying(q))
			return;
		const auto was = g_intimidated.find(q);
		if (was != g_intimidated.end() && now < was->second)
			return;
		if (bow_defiant(q, player))
		{
			// seen: they look at the player (and turn to, if they aren't busy fighting), and go on
			natives::TaskLookAtEntity(q, player, 3000);
			if (!natives::IsPedInCombat(q, 0))
				natives::TaskTurnPedToFaceEntity(q, player, 1500);
			g_intimidated[q] = now + 3000;
		}
		else if (!natives::IsPedHuman(q))
			return;
		else
		{
			if (dist3(c, natives::GetEntityCoords(q, TRUE)) < 14.0f)
				natives::TaskHandsUp(q, 6000, player);
			else
				natives::TaskSmartFleePed(q, player, 120.0f, 12000);
			natives::SetPedKeepTask(q, TRUE);
			g_intimidated[q] = now + 6000;
		}
		for (auto it = g_intimidated.begin(); it != g_intimidated.end();)
			it = now > it->second + 20000 ? g_intimidated.erase(it) : std::next(it);
	}

	// ---- Minecraft's lead on GTA's things, and Minecraft's boats carrying GTA's people ----

	Vector3 v3(float x, float y, float z)
	{
		Vector3 v = {};
		v.x = x;
		v.y = y;
		v.z = z;
		return v;
	}

	float dist3(const Vector3 &a, const Vector3 &b)
	{
		return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
	}

	/// One end of a lead: Steve's hand, a GTA person (by the neck), car or thing (a point on it), a fixed spot (a wall,
	/// the street, a ceiling, one of Minecraft's blocks), or Minecraft's own mobs (Minecraft has that end).
	struct LeashEnd
	{
		int kind = 0;       // 0 Steve's hand, 1 a GTA entity, 2 a fixed spot, 3 Minecraft's mobs
		Entity e = 0;
		int bone = 0;       // a person: the bone it's tied at
		Vector3 local = {}; // a car or a thing: the point on it, in its own frame
		Vector3 world = {}; // a fixed spot
	};
	struct Leash
	{
		int id = 0;
		LeashEnd a, b;       // a: what's on the lead; b: what holds it (Steve's hand, or what it's tied to)
		float length = 4.0f; // slack up to this far (m)
		int nextTaskAt = 0;  // a person led: when to tell them again to walk after
		int correctedAt = 0; // Steve held back: when Minecraft was last told
	};
	std::vector<Leash> g_leashes;
	int g_leashNext = 1;
	bool g_leashesSent = false;
	constexpr size_t kMaxLeashes = 32;
	// Leads never snap. Further than this (a teleport, a pearl, a waypoint): what's on a lead in Steve's hand comes along
	// to him; anything else stays where it is, held
	constexpr float kLeashBring = 30.0f;
	constexpr int kNeckBone = 0x9995;

	// Minecraft's boats near the player, and GTA's people and animals sat in them
	struct McBoat
	{
		int id;
		float x, y, z, yaw; // GTA coordinates; Minecraft's yaw
		float vx, vy, vz;
		int free;
		bool catches; // takes in who walks into it (not while Steve rows it)
	};
	std::vector<McBoat> g_mcBoats;
	int64_t g_mcBoatsNs = 0;
	struct Rider
	{
		int boat = 0;
		bool seated = false;    // Minecraft has given them the seat
		int askedAt = 0;
		float x = 0, y = 0, z = 0, heading = 0; // the seat (GTA coordinates, feet)
		int lostAt = 0;         // Minecraft stopped listing them (the boat broke)
	};
	std::map<Ped, Rider> g_riders;
	std::unordered_map<Ped, int> g_riderFreedAt;
	int g_nextBoatCatchAt = 0;

	/// Steve's hand (GTA coordinates): Minecraft's player while it moves the player, else GTA's.
	Vector3 hand_point(Ped player)
	{
		if (g_walk.on || g_carFly.on)
			return v3(g_walk.x, g_walk.y, g_walk.z + 1.1f);
		const Vector3 p = natives::GetEntityCoords(player, TRUE);
		return v3(p.x, p.y, p.z + 0.1f);
	}

	Vector3 leash_point(Ped player, const LeashEnd &end)
	{
		if (end.kind == 0)
			return hand_point(player);
		if (end.kind == 1)
			return end.bone != 0 ? natives::GetPedBoneCoords(end.e, end.bone)
				: natives::GetOffsetFromEntityInWorldCoords(end.e, end.local.x, end.local.y, end.local.z);
		return end.world;
	}

	/// What moves when an end is pulled: the person, car or thing (a person in a car: the car; Steve in a car: his car), or
	/// 0 (Steve on foot, a fixed spot).
	Entity leash_body(Ped player, const LeashEnd &end)
	{
		if (end.kind == 0)
			return natives::IsPedInAnyVehicle(player, FALSE) ? Entity(natives::GetVehiclePedIsIn(player, FALSE)) : 0;
		if (end.kind != 1)
			return 0;
		if (natives::GetEntityType(end.e) == 1 && natives::IsPedInAnyVehicle(end.e, FALSE))
			return natives::GetVehiclePedIsIn(end.e, FALSE);
		return end.e;
	}

	std::unordered_map<Hash, float> g_massOf;

	/// Roughly how heavy a GTA thing is (kg): people by build (the heavy-set twice a slim one), animals, cars and things by
	/// their size (a bicycle a few kilos).
	float entity_mass(Entity e)
	{
		const Hash model = natives::GetEntityModel(e);
		if (const auto known = g_massOf.find(model); known != g_massOf.end())
			return known->second;
		Vector3 mn = {}, mx = {};
		natives::GetModelDimensions(model, &mn, &mx);
		const float volume = std::max(0.001f, (mx.x - mn.x) * (mx.y - mn.y) * (mx.z - mn.z));
		float kg;
		switch (natives::GetEntityType(e))
		{
		case 1:
		{
			static const std::unordered_set<Hash> heavy = [] {
				std::unordered_set<Hash> s;
				for (const char *m : {"a_f_m_fatbla_01", "a_f_m_fatcult_01", "a_f_m_fatwhite_01", "a_m_m_fatlatin_01", "a_m_m_genfat_01",
						 "a_m_m_genfat_02", "a_m_m_og_boss_01", "s_m_m_bouncer_01", "u_m_y_babyd", "ig_babyd", "a_m_m_salton_01", "a_m_m_salton_03"})
					s.insert(natives::GetHashKey(m));
				return s;
			}();
			kg = !natives::IsPedHuman(e) ? std::clamp(volume * 120.0f, 2.0f, 700.0f) : heavy.count(model) ? 140.0f : 70.0f;
			break;
		}
		case 2:
			kg = natives::IsThisModelABicycle(model) ? 15.0f : std::max(300.0f, volume * 110.0f);
			break;
		default:
			kg = std::clamp(volume * 250.0f, 1.0f, 20000.0f);
			break;
		}
		g_massOf[model] = kg;
		return kg;
	}

	/// How much an end can pull along (kg): Steve 100 (more with Minecraft's strength), a person a little less than their
	/// own weight (none lying down), a car three times its own; a thing or a fixed spot nothing.
	float leash_strength(const LeashEnd &end, Entity body)
	{
		if (end.kind == 0 && body == 0)
		{
			const int strong = mc_effect("strength");
			return 100.0f + (strong >= 0 ? 60.0f * float(strong + 1) : 0.0f);
		}
		if (body == 0 || g_riders.count(body))
			return 0.0f;
		switch (natives::GetEntityType(body))
		{
		case 1:
			return natives::IsPedDeadOrDying(body) || natives::IsPedRagdoll(body) ? 0.0f : entity_mass(body) * 0.9f;
		case 2:
			return entity_mass(body) * 3.0f;
		default:
			return 0.0f;
		}
	}

	/// Whether a lead can move this end at all (not Steve on foot, a fixed spot, or someone sat in a boat).
	bool leash_movable(const LeashEnd &end, Entity body)
	{
		return body != 0 && (end.kind == 0 || end.kind == 1) && !g_riders.count(body);
	}

	Vector3 leash_velocity(Ped player, const LeashEnd &end, Entity body)
	{
		if (end.kind == 0 && body == 0)
			return g_walk.on ? g_drive.vel : natives::GetEntityVelocity(player);
		return body != 0 ? natives::GetEntityVelocity(body) : v3(0, 0, 0);
	}

	/// The end at `p` held back: no further than the lead from the other end at `c` (as at a wall).
	void leash_restrain(Ped player, Leash &l, const LeashEnd &end, Entity body, const Vector3 &p, const Vector3 &c, int now)
	{
		const float d = dist3(p, c);
		if (d <= l.length || d < 1e-3f)
			return;
		const float ux = (p.x - c.x) / d, uy = (p.y - c.y) / d, uz = (p.z - c.z) / d, over = d - l.length;
		if (end.kind == 0 && body == 0)
		{
			// Steve: held back, Minecraft told where he stopped (as at a wall Minecraft didn't know)
			if (g_walk.on)
			{
				g_walk.x -= ux * over;
				g_walk.y -= uy * over;
				if (now - l.correctedAt > 100)
				{
					walk_correct(true, "held by a lead");
					l.correctedAt = now;
				}
			}
			else if (!natives::IsPedRagdoll(player))
			{
				const Vector3 q = natives::GetEntityCoords(player, TRUE);
				natives::SetEntityCoordsNoOffset(player, q.x - ux * over, q.y - uy * over, q.z);
			}
			return;
		}
		if (body == 0 || g_riders.count(body))
			return;
		if (natives::GetEntityType(body) == 1 && !natives::IsPedRagdoll(body) && !natives::IsPedDeadOrDying(body))
		{
			// someone walking (or running) off: they get no further
			const Vector3 q = natives::GetEntityCoords(body, TRUE);
			natives::SetEntityCoordsNoOffset(body, q.x - ux * over, q.y - uy * over, q.z);
			return;
		}
		// a car, a thing, someone lying down: what took it further is gone, and it's drawn back a little
		const Vector3 v = natives::GetEntityVelocity(body);
		const float out = std::max(0.0f, v.x * ux + v.y * uy + v.z * uz) + std::min(over * 3.0f, 6.0f);
		if (natives::GetEntityType(body) == 3)
		{
			if (movable_object(body)) // (the map's own never moved in the first place)
				natives::ApplyForceToEntity(body, -ux * out, -uy * out, -uz * out);
		}
		else
			natives::SetEntityVelocity(body, v.x - ux * out, v.y - uy * out, v.z - uz * out);
	}

	/// `body` drawn toward `c` (from `p`) at `want` m/s at least: along the ground for a person or an animal (dragged on
	/// their back, never lifted: a pull from the hand's height had them floating), and what it moved sideways mostly
	/// taken off (kept, the pull turned it round and round the one pulling).
	void leash_drag(Entity body, const Vector3 &p, const Vector3 &c, float want)
	{
		const int type = natives::GetEntityType(body);
		const Vector3 v = natives::GetEntityVelocity(body);
		const bool ground = type == 1 || (type == 3 && c.z - p.z < 1.5f);
		float ux = c.x - p.x, uy = c.y - p.y, uz = ground ? 0.0f : c.z - p.z;
		const float ul = std::sqrt(ux * ux + uy * uy + uz * uz);
		if (ul < 1e-3f)
			return;
		ux /= ul;
		uy /= ul;
		uz /= ul;
		const float along = v.x * ux + v.y * uy + v.z * uz;
		const float tx = v.x - along * ux, ty = v.y - along * uy, tz = v.z - along * uz;
		const float keep = type == 2 ? 0.6f : 0.15f, now = std::max(along, want);
		float vz = uz * now + tz * keep;
		if (ground)
			vz = std::min(v.z, 0.5f); // (on the ground, as it fell: no lift)
		if (type == 3)
		{
			if (movable_object(body))
				natives::ApplyForceToEntity(body, ux * now + tx * keep - v.x, uy * now + ty * keep - v.y, vz - v.z);
		}
		else
			natives::SetEntityVelocity(body, ux * now + tx * keep, uy * now + ty * keep, vz);
	}

	/// The end `body` (its lead at `p`) pulled along toward `c` by something strong enough, moving off at `speed`.
	void leash_lead(Leash &l, Entity body, const Vector3 &p, const Vector3 &c, float speed, int now)
	{
		const float d = dist3(p, c);
		if (d <= l.length || d < 1e-3f || g_riders.count(body))
			return;
		const float ux = (c.x - p.x) / d, uy = (c.y - p.y) / d, over = d - l.length;
		const int type = natives::GetEntityType(body);
		if (type == 1)
		{
			const bool down = natives::IsPedDeadOrDying(body) || natives::IsPedRagdoll(body);
			if (!down && over < 1.6f && speed < 4.5f)
			{
				// led: walks after (as Minecraft's mobs on a lead do), never more than a little behind
				if (now >= l.nextTaskAt)
				{
					const float stop = l.length * 0.6f;
					natives::TaskGoStraightToCoord(body, c.x - ux * stop, c.y - uy * stop, c.z, over > 0.7f ? 2.0f : 1.0f, 2500, 40000.0f, 0.5f);
					l.nextTaskAt = now + 400;
				}
				if (over > 0.4f)
				{
					const Vector3 q = natives::GetEntityCoords(body, TRUE);
					natives::SetEntityCoordsNoOffset(body, q.x + ux * (over - 0.4f), q.y + uy * (over - 0.4f), q.z);
				}
				return;
			}
			// pulled harder than anyone walks (a car, a sprint): off their feet and dragged along the ground, as long as
			// it goes on (kept down: they'd get up mid-drag)
			if (!down || now >= l.nextTaskAt)
			{
				natives::SetPedToRagdoll(body, 1800);
				l.nextTaskAt = now + 1000;
			}
		}
		else if (type == 3)
		{
			// signs, bins, doors: torn loose (the map's own swapped for the plugin's copy, which the lead is on from now)
			const Object o = loosen(body);
			if (o == 0)
				return;
			if (Entity(o) != body)
			{
				for (LeashEnd *end : {&l.a, &l.b})
					if (end->kind == 1 && end->e == body)
						end->e = o;
				body = o;
			}
			natives::FreezeEntityPosition(body, FALSE);
			natives::SetEntityDynamic(body, TRUE);
		}
		leash_drag(body, p, c, std::min(15.0f, speed + over * 2.5f));
	}

	/// Two of GTA's people, animals, cars or things on one lead (a dog tied to someone): pulled taut, each gives way by the
	/// other's weight (the light one is dragged most, a heavy one hardly moves). Someone on their feet is tugged a step,
	/// or off their feet by a hard pull; the rest are dragged.
	void leash_mutual(Leash &l, Entity bodyA, Entity bodyB, const Vector3 &pa, const Vector3 &pb, float d, int now)
	{
		const float over = d - l.length;
		const Vector3 va = natives::GetEntityVelocity(bodyA), vb = natives::GetEntityVelocity(bodyB);
		const float ux = (pb.x - pa.x) / d, uy = (pb.y - pa.y) / d, uz = (pb.z - pa.z) / d;
		const float apart = std::max(0.0f, (vb.x - va.x) * ux + (vb.y - va.y) * uy + (vb.z - va.z) * uz); // (how fast they part)
		const float mA = std::max(1.0f, entity_mass(bodyA)), mB = std::max(1.0f, entity_mass(bodyB));
		auto give = [&](Entity body, const Vector3 &p, const Vector3 &c, float share) {
			const float amount = over * share, speed = apart * share;
			if (amount < 0.02f)
				return;
			if (natives::GetEntityType(body) == 1 && !natives::IsPedDeadOrDying(body) && !natives::IsPedRagdoll(body))
			{
				if (amount < 1.2f && speed < 3.0f)
				{
					// a tug: a step their way
					const float hx = c.x - p.x, hy = c.y - p.y, hl = std::max(1e-3f, std::sqrt(hx * hx + hy * hy));
					const Vector3 q = natives::GetEntityCoords(body, TRUE);
					const float step = std::min(amount, 0.08f);
					natives::SetEntityCoordsNoOffset(body, q.x + hx / hl * step, q.y + hy / hl * step, q.z);
					return;
				}
				natives::SetPedToRagdoll(body, 1800); // (yanked off their feet)
			}
			else if (natives::GetEntityType(body) == 1 && now >= l.nextTaskAt && amount > 0.5f)
			{
				natives::SetPedToRagdoll(body, 1800);
				l.nextTaskAt = now + 1000;
			}
			leash_drag(body, p, c, std::min(12.0f, speed + amount * 2.5f));
		};
		give(bodyA, pa, pb, mB / (mA + mB));
		give(bodyB, pb, pa, mA / (mA + mB));
	}

	/// A taut lead: the end moving off (or, both still, the stronger) pulls; it moves the other end along if it's strong
	/// enough for its weight (a slim person yes, a heavy-set one or a car not, for Steve), else it's held back itself.
	void leash_pull(Ped player, Leash &l, const Vector3 &pa, const Vector3 &pb, float d, int now)
	{
		const Entity bodyA = leash_body(player, l.a), bodyB = leash_body(player, l.b);
		if (l.a.kind == 1 && l.b.kind == 1 && bodyA != bodyB && leash_movable(l.a, bodyA) && leash_movable(l.b, bodyB) &&
			(natives::GetEntityType(bodyA) != 3 || movable_object(bodyA)) && (natives::GetEntityType(bodyB) != 3 || movable_object(bodyB)))
		{
			leash_mutual(l, bodyA, bodyB, pa, pb, d, now);
			return;
		}
		const Vector3 va = leash_velocity(player, l.a, bodyA), vb = leash_velocity(player, l.b, bodyB);
		const float ux = (pb.x - pa.x) / d, uy = (pb.y - pa.y) / d, uz = (pb.z - pa.z) / d;
		const float outA = -(va.x * ux + va.y * uy + va.z * uz), outB = vb.x * ux + vb.y * uy + vb.z * uz;
		const float sA = leash_strength(l.a, bodyA), sB = leash_strength(l.b, bodyB);
		const bool still = std::fabs(outA - outB) <= 0.25f;
		const bool aPulls = still ? sA >= sB : outA > outB;
		const LeashEnd &puller = aPulls ? l.a : l.b, &other = aPulls ? l.b : l.a;
		const Entity pBody = aPulls ? bodyA : bodyB, oBody = aPulls ? bodyB : bodyA;
		const Vector3 &pp = aPulls ? pa : pb, &op = aPulls ? pb : pa;
		const float strength = aPulls ? sA : sB, speed = std::max(0.0f, aPulls ? outA : outB);
		if (leash_movable(other, oBody) && strength > 0.0f && strength >= entity_mass(oBody))
			leash_lead(l, oBody, op, pp, speed, now);
		else if (still)
		{
			// nobody moving off, and the stronger can't draw the other in: it stays as it is
		}
		else
			leash_restrain(player, l, puller, pBody, pp, op, now);
	}

	void leash_event(const Leash &l, const char *what, const Vector3 &at)
	{
		logf("lead %d %s (on %d kind %d, held by %d kind %d, %.1f m long)", l.id, what, int(l.a.e), l.a.kind, int(l.b.e), l.b.kind, l.length);
		sendf("{\"t\":\"leashevt\",\"id\":%d,\"e\":\"%s\",\"pos\":[%.3f,%.3f,%.3f]}", l.id, what, at.x, at.z + g_yOffset, -at.y);
	}

	/// Every lead's ends for Minecraft, which draws them ({"t":"leashes","l":[[id,kind,ax,ay,az,bx,by,bz],...]}, Minecraft
	/// coordinates; kind 0 in Steve's hand, 1 tied to b, 2 Minecraft's mobs tied to b), every frame while there are any.
	void leash_send(Ped player)
	{
		if (g_leashes.empty() && !g_leashesSent)
			return;
		std::string out = "{\"t\":\"leashes\",\"l\":[";
		for (const Leash &l : g_leashes)
		{
			const int kind = l.b.kind == 0 ? 0 : l.a.kind == 3 ? 2 : 1;
			const Vector3 pb = leash_point(player, l.b), pa = l.a.kind == 3 ? pb : leash_point(player, l.a);
			char e[160];
			snprintf(e, sizeof(e), "%s[%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f]", out.back() == '[' ? "" : ",", l.id, kind, pa.x, pa.z + g_yOffset, -pa.y, pb.x,
				pb.z + g_yOffset, -pb.y);
			out += e;
		}
		g_ws.send(out + "]}");
		g_leashesSent = !g_leashes.empty();
	}

	/// Every frame: leads whose thing is gone drop, overstretched ones snap, taut ones pull (leash_pull); Minecraft draws them.
	void leash_tick(Ped player)
	{
		const int now = natives::GetGameTimer();
		for (size_t i = 0; i < g_leashes.size();)
		{
			Leash &l = g_leashes[i];
			const bool aOk = l.a.kind != 1 || natives::DoesEntityExist(l.a.e), bOk = l.b.kind != 1 || natives::DoesEntityExist(l.b.e);
			if (!aOk || !bOk)
			{
				// what it was on is gone (cleaned up by GTA, deleted): the lead drops
				const LeashEnd &left = aOk && l.a.kind != 3 ? l.a : l.b;
				if (left.kind != 1 || natives::DoesEntityExist(left.e))
					leash_event(l, "drop", leash_point(player, left));
				g_leashes.erase(g_leashes.begin() + i);
				continue;
			}
			if (l.a.kind == 3)
			{
				++i; // (Minecraft's mobs: its own lead pulls them)
				continue;
			}
			const Vector3 pa = leash_point(player, l.a), pb = leash_point(player, l.b);
			const float d = dist3(pa, pb);
			if (d > kLeashBring)
			{
				// (never snaps) a teleport: who's on a lead in Steve's hand is brought along beside him
				const Entity body = leash_body(player, l.a);
				if (l.b.kind == 0 && l.a.kind == 1 && body != 0 && natives::GetEntityType(body) != 2 && !g_riders.count(body) &&
					(natives::GetEntityType(body) != 3 || movable_object(body)))
				{
					const float back = std::max(1.0f, l.length * 0.7f);
					const float hx = pa.x - pb.x, hy = pa.y - pb.y, hl = std::max(1e-3f, std::sqrt(hx * hx + hy * hy));
					natives::SetEntityCoordsNoOffset(body, pb.x + hx / hl * back, pb.y + hy / hl * back, pb.z - 0.1f);
					natives::SetEntityVelocity(body, 0.0f, 0.0f, 0.0f);
					logf("lead %d: %.0f m off: brought along", l.id, d);
				}
				++i;
				continue;
			}
			if (d > l.length && !g_noInput)
				leash_pull(player, l, pa, pb, d, now);
			++i;
		}
		leash_send(player);
	}

	/// What the crosshair is on within `reach` of Steve: a person, car or thing (`hit`), or a surface of the map or of one of
	/// Minecraft's blocks (hit 0), where (`at`) and facing (`n`). False if there's nothing.
	bool aim_target(Ped player, float reach, Entity &hit, Vector3 &at, Vector3 &n)
	{
		const Vector3 r = natives::GetFinalRenderedCamRot(2), eye = natives::GetFinalRenderedCamCoord();
		const float h = r.z * 3.14159265f / 180.0f, pt = r.x * 3.14159265f / 180.0f;
		const float ux = -std::sin(h) * std::cos(pt), uy = std::cos(h) * std::cos(pt), uz = std::sin(pt);
		const Vector3 me = hand_point(player);
		const float along = std::max(0.0f, (me.x - eye.x) * ux + (me.y - eye.y) * uy + (me.z - eye.z) * uz - 0.5f);
		const float sx = eye.x + ux * along, sy = eye.y + uy * along, sz = eye.z + uz * along, span = reach + 1.0f;
		const Entity ignore = natives::IsPedInAnyVehicle(player, FALSE) ? Entity(natives::GetVehiclePedIsIn(player, FALSE)) : Entity(player);
		BOOL any = FALSE;
		hit = 0;
		const int probe = natives::StartShapeTestLosProbe(sx, sy, sz, sx + ux * span, sy + uy * span, sz + uz * span, 1 | 2 | 4 | 8 | 16, ignore, 7);
		if (natives::GetShapeTestResult(probe, &any, &at, &n, &hit) != 2)
			any = FALSE;
		if (any && hit != 0 && (g_props.handles.count(hit) || g_doublePeds.count(hit) || natives::IsEntityAttached(hit)))
			hit = 0; // (one of Minecraft's blocks: a fixed spot; a mob's double or something carried: not a thing of its own)
		if (hit == 0 || natives::GetEntityType(hit) != 1)
		{
			// the crosshair just past someone (a person is thin): whoever is closest to its line, within a hand's breadth
			const float limit = any ? std::sqrt((at.x - sx) * (at.x - sx) + (at.y - sy) * (at.y - sy) + (at.z - sz) * (at.z - sz)) : span;
			float best = 0.35f;
			int handles[256];
			const int count = worldGetAllPeds(handles, 256);
			for (int i = 0; i < count; ++i)
			{
				const Ped q = handles[i];
				if (q == player || g_doublePeds.count(q))
					continue;
				const Vector3 o = natives::GetEntityCoords(q, TRUE);
				const float px = o.x - sx, py = o.y - sy, pz = o.z + 0.2f - sz, t = px * ux + py * uy + pz * uz;
				if (t < 0.0f || t > limit + 0.3f)
					continue;
				const float lx = px - ux * t, ly = py - uy * t, lz = (pz - uz * t) * 0.5f, off = std::sqrt(lx * lx + ly * ly + lz * lz);
				if (off < best)
				{
					best = off;
					hit = q;
					at = v3(o.x - lx, o.y - ly, o.z);
					any = TRUE;
				}
			}
		}
		return any != FALSE;
	}

	/// The end of a lead at what the crosshair is on: a person by the neck, a car or thing at that point, or the spot.
	LeashEnd leash_end_at(Entity hit, const Vector3 &at, const Vector3 &n)
	{
		LeashEnd end;
		if (hit == 0)
		{
			end.kind = 2;
			end.world = v3(at.x + n.x * 0.05f, at.y + n.y * 0.05f, at.z + n.z * 0.05f);
			return end;
		}
		end.kind = 1;
		end.e = hit;
		if (natives::GetEntityType(hit) == 1)
		{
			const Vector3 neck = natives::GetPedBoneCoords(hit, kNeckBone), o = natives::GetEntityCoords(hit, TRUE);
			if (dist3(neck, o) < 1.5f)
			{
				end.bone = kNeckBone;
				return end;
			}
		}
		end.local = natives::GetOffsetFromEntityGivenWorldCoords(hit, at.x, at.y, at.z);
		return end;
	}

	/// Right click with a lead or shears in Minecraft at GTA's world ({"t":"leashuse","item":"lead"|"shears","mobs":n}; n:
	/// Minecraft's own mobs on leads in Steve's hand). With leads in hand: they're tied to what the crosshair is on (a
	/// person, car, thing, wall, the street, a ceiling); the thing one is on again: let go. Nothing in hand: what's tied
	/// there comes back into the hand, or a new lead goes on that person, car or thing. Shears: its leads are cut.
	void leash_use(Ped player, const std::string &m)
	{
		const std::string item = json_str(m, "item");
		const int mobs = int(json_num(m, "mobs", 0.0));
		if (g_noInput)
		{
			logf("lead: right click with %s ignored (no input now)", item.c_str());
			return;
		}
		Entity hit = 0;
		Vector3 at = {}, n = {};
		// (within a lead's reach whatever /range is: put on something further off, it would snap at once)
		const bool any = aim_target(player, std::clamp(g_meleeRange, 6.0f, 9.0f), hit, at, n);
		{
			const Vector3 me = hand_point(player);
			logf("lead: right click with %s (%d of Minecraft's mobs in hand, %d leads): %s %d type %d model %08X, %.1f m off", item.c_str(),
				mobs, int(g_leashes.size()), any ? "on" : "nothing", int(hit), hit != 0 ? natives::GetEntityType(hit) : 0,
				hit != 0 ? unsigned(natives::GetEntityModel(hit)) : 0u, any ? dist3(me, at) : 0.0f);
		}
		if (!any)
			return; // (the sky: nothing to tie to)
		auto on = [&](const LeashEnd &e) {
			return (hit != 0 && e.kind == 1 && e.e == hit) || (hit == 0 && e.kind == 2 && dist3(e.world, at) < 1.2f);
		};
		if (item == "shears")
		{
			for (size_t i = 0; i < g_leashes.size();)
			{
				const Leash &l = g_leashes[i];
				if (on(l.a) || on(l.b))
				{
					leash_event(l, "cut", leash_point(player, l.a.kind == 3 ? l.b : l.a));
					g_leashes.erase(g_leashes.begin() + i);
				}
				else
					++i;
			}
			return;
		}
		bool holding = mobs > 0;
		for (const Leash &l : g_leashes)
			holding = holding || l.b.kind == 0;
		if (holding)
		{
			const LeashEnd to = leash_end_at(hit, at, n);
			for (size_t i = g_leashes.size(); i-- > 0;)
			{
				Leash &l = g_leashes[i];
				if (l.b.kind != 0)
					continue;
				if (on(l.a))
				{
					// the thing itself again: let go of it
					leash_event(l, "drop", leash_point(player, l.a));
					g_leashes.erase(g_leashes.begin() + i);
					continue;
				}
				const Vector3 pa = leash_point(player, l.a), pb = leash_point(player, to);
				if (dist3(pa, pb) > kLeashBring - 1.0f)
					continue; // (too far from what's on it: it stays in hand)
				l.b = to;
				l.length = std::clamp(dist3(pa, pb), 1.0f, 9.0f);
				leash_event(l, "tied", pb);
			}
			if (mobs > 0 && g_leashes.size() < kMaxLeashes)
			{
				Leash l;
				l.id = g_leashNext++;
				l.a.kind = 3;
				l.b = to;
				g_leashes.push_back(l);
				leash_send(player); // (Minecraft has the spot before it ties its mobs there)
				leash_event(l, "tiemobs", leash_point(player, l.b));
			}
			return;
		}
		// nothing in hand: what's tied here comes back into it (as from a fence knot)
		bool took = false;
		const Vector3 hand = hand_point(player);
		for (size_t i = 0; i < g_leashes.size();)
		{
			Leash &l = g_leashes[i];
			if (l.a.kind == 3 && on(l.b))
			{
				leash_event(l, "takemobs", leash_point(player, l.b));
				g_leashes.erase(g_leashes.begin() + i);
				took = true;
				continue;
			}
			if (l.a.kind != 3 && l.b.kind != 0 && (on(l.a) || on(l.b)))
			{
				l.b = LeashEnd();
				const Vector3 pa = leash_point(player, l.a);
				l.length = std::clamp(dist3(pa, hand), 2.0f, 9.0f);
				leash_event(l, "taken", pa);
				took = true;
			}
			++i;
		}
		if (took || hit == 0 || g_leashes.size() >= kMaxLeashes)
			return;
		const int type = natives::GetEntityType(hit);
		if (type < 1 || type > 3)
			return;
		// a new lead on that person, car or thing, in Steve's hand
		Leash l;
		l.id = g_leashNext++;
		l.a = leash_end_at(hit, at, n);
		const Vector3 pa = leash_point(player, l.a);
		l.length = std::clamp(dist3(pa, hand), 2.0f, 9.0f);
		g_leashes.push_back(l);
		leash_send(player);
		leash_event(l, "new", pa);
	}

	/// Minecraft's mobs tied at one of GTA's spots are all off it ({"t":"leashgone","id":n}): the spot goes.
	void leash_gone(int id)
	{
		g_leashes.erase(std::remove_if(g_leashes.begin(), g_leashes.end(), [&](const Leash &l) { return l.id == id && l.a.kind == 3; }),
			g_leashes.end());
	}

	/// Every lead gone (the passthrough off, /clearall); `drop`: each drops its lead in Minecraft.
	void leash_clear_all(Ped player, bool drop)
	{
		if (g_leashes.empty())
			return;
		if (drop)
			for (const Leash &l : g_leashes)
				if (l.a.kind != 3 && (l.a.kind != 1 || natives::DoesEntityExist(l.a.e)))
					leash_event(l, "drop", leash_point(player, l.a));
		g_leashes.clear();
		g_leashesSent = g_ws.connected();
		leash_send(player); // (none left: Minecraft lets its ends go)
	}

	/// Whether GTA's point p (a cell's middle) is inside its solid world (a wall, a roof, under the street) rather than in
	/// the open (a room, under a bridge). Looking straight up: the underside of something means open air under it; the
	/// top of something seen from within, or GTA's ground above with nothing seen (its collision faces only outward),
	/// means inside.
	bool gta_solid(const Vector3 &p)
	{
		BOOL hit = FALSE;
		Vector3 at = {}, n = {};
		Entity e = 0;
		const int probe = natives::StartShapeTestLosProbe(p.x, p.y, p.z, p.x, p.y, p.z + 60.0f, 1, 0, 7);
		if (natives::GetShapeTestResult(probe, &hit, &at, &n, &e) == 2 && hit)
		{
			if (n.z < -0.3f)
				return false;
			if (n.z > 0.3f)
				return true;
		}
		float top = 0.0f;
		return natives::GetGroundZFor3dCoord(p.x, p.y, p.z + 100.0f, &top, FALSE, FALSE) && top > p.z;
	}

	/// The rows of numbers of a JSON array of arrays ("key":[[1,2],[3,4]]).
	std::vector<std::vector<double>> json_rows(const std::string &m, const char *key)
	{
		std::vector<std::vector<double>> rows;
		const char *p = json_value(m, key);
		if (p == nullptr || *p != '[')
			return rows;
		++p;
		while (*p != '\0')
		{
			while (*p == ' ' || *p == ',')
				++p;
			if (*p != '[')
				break;
			++p;
			std::vector<double> row;
			while (*p != '\0' && *p != ']')
			{
				char *end = nullptr;
				const double v = std::strtod(p, &end);
				if (end == p)
				{
					++p;
					continue;
				}
				row.push_back(v);
				p = end;
			}
			if (*p == ']')
				++p;
			rows.push_back(std::move(row));
		}
		return rows;
	}

	/// {"t":"boats","b":[[id,x,y,z,yaw,vx,vy,vz,free,catches],...],"s":[[ped,boat,x,y,z,yaw],...]} (Minecraft coordinates):
	/// Minecraft's boats near the player, and the seat of each of GTA's people sat in one, every tick while there are any.
	void boats_message(const std::string &m)
	{
		g_mcBoats.clear();
		for (const auto &r : json_rows(m, "b"))
			if (r.size() >= 10)
				g_mcBoats.push_back({int(r[0]), float(r[1]), float(-r[3]), float(r[2]) - g_yOffset, float(r[4]), float(r[5]), float(-r[7]),
					float(r[6]), int(r[8]), r[9] != 0.0});
		g_mcBoatsNs = now_nanos();
		const int now = natives::GetGameTimer();
		std::set<Ped> seen;
		for (const auto &r : json_rows(m, "s"))
		{
			if (r.size() < 6)
				continue;
			const auto it = g_riders.find(Ped(r[0]));
			if (it == g_riders.end())
				continue;
			Rider &rd = it->second;
			rd.boat = int(r[1]);
			rd.seated = true;
			rd.lostAt = 0;
			rd.x = float(r[2]);
			rd.y = float(-r[4]);
			rd.z = float(r[3]) - g_yOffset;
			rd.heading = wrap_degrees(180.0f - float(r[5]));
			seen.insert(it->first);
		}
		for (auto &[ped, rd] : g_riders)
			if (rd.seated && !seen.count(ped) && rd.lostAt == 0)
				rd.lostAt = now; // (the boat broke: out they come, in a moment)
	}

	/// Someone sat in a boat gets out (the boat broke, they died, the passthrough is off); `tell`: Minecraft frees the seat.
	void rider_free(Ped ped, bool tell)
	{
		if (natives::DoesEntityExist(ped))
		{
			natives::FreezeEntityPosition(ped, FALSE);
			natives::SetEntityCollision(ped, TRUE, TRUE);
			natives::ClearPedTasksImmediately(ped);
			natives::SetBlockingOfNonTemporaryEvents(ped, FALSE);
			natives::SetPedCanRagdoll(ped, TRUE);
		}
		if (tell)
			sendf("{\"t\":\"boatleave\",\"ped\":%d}", ped);
		g_riderFreedAt[ped] = natives::GetGameTimer();
		if (g_riderFreedAt.size() > 64) // (only the last few seconds' matter: it grew for ever)
			for (auto it = g_riderFreedAt.begin(); it != g_riderFreedAt.end();)
				it = natives::GetGameTimer() - it->second > 3000 ? g_riderFreedAt.erase(it) : std::next(it);
	}

	void riders_clear_all(bool tell)
	{
		for (const auto &[ped, rd] : g_riders)
			rider_free(ped, tell);
		g_riders.clear();
		g_mcBoats.clear();
	}

	/// Every frame: GTA's people and animals in Minecraft's boats sit where their seat is (carried with the boat); ten
	/// times a second, a boat with room that Steve isn't rowing takes in whoever walks into it, as Minecraft's boats do.
	void boats_tick(Ped player)
	{
		if (g_riders.empty() && g_mcBoats.empty())
			return;
		const int now = natives::GetGameTimer();
		const float dt = std::clamp(float(now_nanos() - g_mcBoatsNs) * 1e-9f, 0.0f, 0.1f);
		static const char *const kSitDict = "amb@world_human_picnic@male@base", *const kSitAnim = "base";
		for (auto it = g_riders.begin(); it != g_riders.end();)
		{
			const Ped ped = it->first;
			Rider &rd = it->second;
			const bool gone = !natives::DoesEntityExist(ped);
			const bool lost = rd.seated && rd.lostAt != 0 && now - rd.lostAt > 300;
			if (gone || lost || natives::IsPedDeadOrDying(ped) || (!rd.seated && now - rd.askedAt > 1500))
			{
				rider_free(ped, !lost); // (a broken boat has freed its seats itself)
				it = g_riders.erase(it);
				continue;
			}
			if (rd.seated)
			{
				float vx = 0.0f, vy = 0.0f, vz = 0.0f;
				for (const McBoat &b : g_mcBoats)
					if (b.id == rd.boat)
					{
						vx = b.vx;
						vy = b.vy;
						vz = b.vz;
					}
				Vector3 mn = {}, mx = {};
				natives::GetModelDimensions(natives::GetEntityModel(ped), &mn, &mx);
				natives::FreezeEntityPosition(ped, TRUE);
				natives::SetEntityCollision(ped, FALSE, FALSE);
				natives::SetBlockingOfNonTemporaryEvents(ped, TRUE);
				natives::SetPedCanRagdoll(ped, FALSE);
				natives::SetEntityCoordsNoOffset(ped, rd.x + vx * dt, rd.y + vy * dt, rd.z + vz * dt - std::min(mn.z, -0.3f));
				natives::SetEntityHeading(ped, rd.heading);
				if (natives::IsPedHuman(ped) && !natives::IsEntityPlayingAnim(ped, kSitDict, kSitAnim))
				{
					natives::RequestAnimDict(kSitDict);
					if (natives::HasAnimDictLoaded(kSitDict))
						natives::TaskPlayAnimLoop(ped, kSitDict, kSitAnim);
				}
			}
			++it;
		}
		if (g_mcBoats.empty() || g_noInput || now < g_nextBoatCatchAt)
			return;
		g_nextBoatCatchAt = now + 100;
		int handles[256];
		const int count = worldGetAllPeds(handles, 256);
		for (const McBoat &b : g_mcBoats)
		{
			int room = b.free;
			for (const auto &[ped, rd] : g_riders)
				room -= rd.boat == b.id && !rd.seated ? 1 : 0;
			if (!b.catches || room <= 0)
				continue;
			for (int i = 0; i < count && room > 0; ++i)
			{
				const Ped q = handles[i];
				if (q == player || g_doublePeds.count(q) || g_riders.count(q) || natives::IsPedDeadOrDying(q) || natives::IsPedInAnyVehicle(q, FALSE) ||
					natives::IsEntityAMissionEntity(q))
					continue;
				if (const auto freed = g_riderFreedAt.find(q); freed != g_riderFreedAt.end() && now - freed->second < 3000)
					continue;
				const Vector3 o = natives::GetEntityCoords(q, TRUE);
				if (std::fabs(o.x - b.x) > 1.2f || std::fabs(o.y - b.y) > 1.2f)
					continue;
				// Minecraft's boat: a box 1.375 across and 0.5625 high (and 0.2 of reach); a person ~0.3 round
				Vector3 mn = {}, mx = {};
				natives::GetModelDimensions(natives::GetEntityModel(q), &mn, &mx);
				const float feet = o.z + std::min(mn.z, -0.3f);
				if (std::fabs(o.x - b.x) > 0.6875f + 0.2f + 0.3f || std::fabs(o.y - b.y) > 0.6875f + 0.2f + 0.3f || feet < b.z - 0.6f || feet > b.z + 0.8f)
					continue;
				Rider rd;
				rd.boat = b.id;
				rd.askedAt = now;
				g_riders[q] = rd;
				sendf("{\"t\":\"boatgrab\",\"boat\":%d,\"ped\":%d}", b.id, q);
				--room;
			}
		}
		for (auto it = g_riderFreedAt.begin(); it != g_riderFreedAt.end();)
			it = now - it->second > 10000 ? g_riderFreedAt.erase(it) : std::next(it);
	}

	void enderthief_message(Ped player, const std::string &m)
	{
		const char *pos = json_value(m, "pos");
		double x, y, z;
		const int id = int(json_num(m, "id", -1));
		if (!pos || sscanf_s(pos, "[%lf ,%lf ,%lf ]", &x, &y, &z) != 3 || g_noInput)
			return;
		for (const Thief &t : g_thieves)
			if (t.mob == id)
				return;
		const float gx = float(x), gy = float(-z), gz = float(y) - g_yOffset;
		const Vehicle mine = natives::IsPedInAnyVehicle(player, FALSE) ? natives::GetVehiclePedIsIn(player, FALSE) : 0;
		Vehicle best = 0;
		float bestD = 16.0f;
		each_vehicle_near(gx, gy, gz, 16.0f, [&](Vehicle v, float, float, float d) {
			if (v == mine || d >= bestD || natives::IsEntityAMissionEntity(v) || natives::IsEntityDead(v))
				return;
			for (const Thief &t : g_thieves)
				if (t.car == v)
					return;
			best = v;
			bestD = d;
		});
		if (best == 0)
			return;
		static const Hash model = natives::GetHashKey("a_m_y_skater_01");
		natives::RequestModel(model);
		if (!natives::HasModelLoaded(model))
		{
			later([player, m] { enderthief_message(player, m); });
			return;
		}
		const Ped old = natives::GetPedInVehicleSeat(best, -1);
		if (old != 0 && old != player)
		{
			// thrown out at once (the enderman teleported in): the seat must be free for the thief this same moment
			const Vector3 c = natives::GetOffsetFromEntityInWorldCoords(best, -2.2f, 0.0f, 0.0f);
			natives::ClearPedTasksImmediately(old);
			natives::SetEntityCoordsNoOffset(old, c.x, c.y, c.z);
			natives::SetPedToRagdoll(old, 1500);
		}
		natives::SetEntityAsMissionEntity(best);
		const Ped driver = natives::CreatePedInsideVehicle(best, model, -1);
		if (driver == 0)
		{
			Entity car = best;
			natives::SetEntityAsNoLongerNeeded(&car); // (not kept for ever)
			return;
		}
		natives::SetEntityVisible(driver, FALSE, FALSE); // (Minecraft's enderman is who's seen at the wheel)
		natives::SetBlockingOfNonTemporaryEvents(driver, TRUE);
		natives::SetVehicleEngineOn(best, TRUE);
		natives::TaskVehicleDriveWander(driver, best, 32.0f, 786469); // reckless: through red lights, round traffic
		g_thieves.push_back({id, best, driver});
		natives::Notify("An ~p~enderman~s~ stole a car!");
	}

	// People /kill @e left alive in their seats: looked at again shortly, and if still alive, out of the seat and killed
	struct KillCheck
	{
		Ped ped;
		int at;
	};
	std::vector<KillCheck> g_killChecks;

	/// Killed outright (Minecraft's /kill). Damage alone can leave GTA's people sitting in their cars alive: they're looked
	/// at again shortly after (kill_checks_tick).
	void kill_ped(Ped q, int now)
	{
		natives::ApplyDamageToPed(q, 100000);
		natives::SetEntityHealth(q, 0);
		if (natives::IsPedInAnyVehicle(q, FALSE))
			g_killChecks.push_back({q, now + 300});
	}

	void kill_checks_tick(int now)
	{
		for (size_t i = 0; i < g_killChecks.size();)
		{
			const KillCheck k = g_killChecks[i];
			if (now < k.at)
			{
				++i;
				continue;
			}
			g_killChecks.erase(g_killChecks.begin() + i);
			if (!natives::DoesEntityExist(k.ped) || natives::IsPedDeadOrDying(k.ped) || natives::IsEntityDead(k.ped))
				continue;
			// still alive in the seat: pulled out of it, and killed beside the car
			logf("kill: %d still alive in a car: taken out and killed", int(k.ped));
			natives::ClearPedTasksImmediately(k.ped);
			natives::ApplyDamageToPed(k.ped, 100000);
			natives::SetEntityHealth(k.ped, 0);
		}
	}

	/// Every few frames: each stolen car's enderman goes where the car is; a wrecked or far-off car is let go.
	void thieves_tick(Ped player)
	{
		if (g_thieves.empty() || natives::GetFrameCount() % 2 != 0)
			return;
		const Vector3 me = natives::GetEntityCoords(player, TRUE);
		for (auto it = g_thieves.begin(); it != g_thieves.end();)
		{
			const bool exists = natives::DoesEntityExist(it->car) && !natives::IsEntityDead(it->car);
			const Vector3 o = exists ? natives::GetEntityCoords(it->car, TRUE) : Vector3{};
			const bool keep = exists && natives::DoesEntityExist(it->driver) && natives::IsPedInAnyVehicle(it->driver, FALSE) &&
				(o.x - me.x) * (o.x - me.x) + (o.y - me.y) * (o.y - me.y) < 200.0f * 200.0f;
			if (!keep)
			{
				sendf("{\"t\":\"enderride\",\"id\":%d,\"end\":true}", it->mob);
				Entity d = it->driver, c = it->car;
				if (natives::DoesEntityExist(d))
					natives::DeleteEntity(&d);
				if (natives::DoesEntityExist(c))
					natives::SetEntityAsNoLongerNeeded(&c);
				it = g_thieves.erase(it);
				continue;
			}
			// the enderman sits in the driver's seat (Minecraft's feet: a little under the car's middle)
			const Vector3 seat = natives::GetPedBoneCoords(it->driver, 0x2E28);
			sendf("{\"t\":\"enderride\",\"id\":%d,\"pos\":[%.3f,%.3f,%.3f],\"yaw\":%.1f}", it->mob, seat.x, seat.z - 1.2f + g_yOffset, -seat.y,
				wrap_degrees(180.0f - natives::GetEntityHeading(it->car)));
			++it;
		}
	}

	void command_message(Ped player, const std::string &m)
	{
		const std::string c = json_str(m, "c");
		const Vector3 me = natives::GetEntityCoords(player, TRUE);
		if (c == "gta")
		{
			gta_command(player, json_str(m, "a"));
			return;
		}
		if (c == "range")
		{
			g_meleeRange = std::clamp(float(json_num(m, "r", 8.0)), 1.0f, 512.0f);
			return;
		}
		if (c == "tune")
		{
			tune(player, json_str(m, "a"));
			return;
		}
		if (c == "summon")
		{
			summon(player, json_str(m, "what"));
			return;
		}
		if (c == "waypoint")
		{
			// to the waypoint set on GTA's map (in a car: the car with the player in it), on the ground there
			if (!natives::IsWaypointActive())
			{
				natives::Notify("No waypoint: set one on GTA's map first");
				return;
			}
			const Vector3 w = natives::GetBlipInfoIdCoord(natives::GetFirstBlipInfoId(8));
			land_start(player, w.x, w.y, w.z > 1.0f ? w.z : 40.0f, true, "To the ~y~waypoint");
		}
		else if (c == "fly")
		{
			g_flyAllowed = m.find("\"on\":true") != std::string::npos;
			natives::Notify(g_flyAllowed ? "Flying ~g~on~s~ (in a car: ~y~Space~s~ twice)" : "Flying ~r~off");
		}
		else if (c == "kill")
		{
			// "what": all, people, animals, or one GTA model; "not": all but that (people, animals, a model); r0..r: how far
			// from the player (Minecraft's distance=); limit: that many, nearest first
			const std::string what = json_str(m, "what"), but = json_str(m, "not");
			auto model_of = [](const std::string &name) -> Hash {
				if (name.empty() || name == "all" || name == "people" || name == "animals")
					return 0;
				const Hash h = natives::GetHashKey(name.c_str());
				return natives::IsModelValid(h) ? h : 1; // (1: a model GTA hasn't: matches nobody)
			};
			const Hash model = model_of(what), notModel = model_of(but);
			const float r0 = float(json_num(m, "r0", 0.0)), r = std::min(250.0f, float(json_num(m, "r", 250.0)));
			const int limit = int(json_num(m, "limit", 1e9));
			auto matches = [](Ped q, const std::string &kind, Hash h) {
				const bool human = natives::IsPedHuman(q) != FALSE;
				if (kind == "all")
					return true;
				if (kind == "people")
					return human;
				if (kind == "animals")
					return !human;
				return h != 0 && natives::GetEntityModel(q) == h;
			};
			std::vector<std::pair<float, Ped>> victims;
			int handles[256];
			const int n = worldGetAllPeds(handles, 256);
			for (int i = 0; i < n; ++i)
			{
				const Ped q = handles[i];
				if (q == player || g_doublePeds.count(q) || natives::IsPedDeadOrDying(q))
					continue;
				const Vector3 o = natives::GetEntityCoords(q, TRUE);
				const float d = std::sqrt((o.x - me.x) * (o.x - me.x) + (o.y - me.y) * (o.y - me.y) + (o.z - me.z) * (o.z - me.z));
				if (d > r || d < r0 || !matches(q, what, model) || (!but.empty() && matches(q, but, notModel)))
					continue;
				victims.push_back({d, q});
			}
			std::sort(victims.begin(), victims.end());
			int killed = 0;
			for (const auto &[d, q] : victims)
			{
				if (killed >= limit)
					break;
				kill_ped(q, natives::GetGameTimer());
				++killed;
			}
			logf("kill %s: %d of GTA's killed, %d of them in cars", what.c_str(), killed, int(g_killChecks.size()));
			char note[96];
			snprintf(note, sizeof(note), "~r~%d~s~ of GTA's killed", killed);
			natives::Notify(note);
		}
		else if (c == "time")
			natives::SetClockTime(int(json_num(m, "h", 12.0)) % 24, int(json_num(m, "m", 0.0)) % 60, 0);
		else if (c == "weather")
		{
			const std::string w = json_str(m, "w");
			natives::SetWeatherTypeOvertimePersist(w == "thunder" ? "THUNDER" : w == "rain" ? "RAIN" : "CLEAR", 3.0f);
		}
		else if (c == "clearall")
		{
			props_clear_all();
			mobwar_clear_all();
			army_clear();
			g_animalQueue.clear();
			g_hot.clear();
			g_hotClusters.clear();
			g_water.clear();
			g_waterClusters.clear();
			leash_clear_all(player, false);
			riders_clear_all(false);
			loose_clear(); // (GTA's things knocked loose, a door torn off: back where they stood)
			// GTA's own cars, people and fires stay (they may be a mission's); soft: a mission restarted, said in chat
			if (json_value(m, "soft") == nullptr)
				natives::Notify("Cleared: Minecraft's mobs, items and blocks, and GTA's things you moved (GTA's cars and people stay)");
		}
	}

	// Free look: degrees per unit of mouse movement GTA reports (per frame), and per second at a pad's full stick
	constexpr float kMouseLook = 6.0f, kPadLook = 220.0f;

	/// Free look off: GTA's own camera renders again, behind the player, looking about where ours did.
	void walk_cam_end()
	{
		if (g_walkCam.cam == 0)
			return;
		if (!g_drive.on && g_aimCam == 0)
			natives::RenderScriptCams(FALSE);
		natives::DestroyCam(g_walkCam.cam);
		g_walkCam.cam = 0;
		natives::SetGameplayCamRelativeHeading(0.0f);
		natives::SetGameplayCamRelativePitch(std::clamp(g_walkCam.pitch, -60.0f, 40.0f), 1.0f);
	}

	/// Minecraft's free look while Minecraft moves the player: our own camera, turned by the mouse (or a pad's right
	/// stick) all the way up and down. First person sits at Steve's eyes; third person behind his head (near, middle or
	/// far, as GTA's), pulled in where something is in the way. GTA's view key (V) cycles them, as it does GTA's own.
	/// `screen`: Minecraft's inventory or chat is open (the mouse is its pointer, the view stays).
	void walk_cam_tick(Ped ped, bool want, bool screen)
	{
		if (!want || !g_settings.freeLook)
		{
			walk_cam_end();
			return;
		}
		const int64_t now = now_nanos();
		if (g_walkCam.cam == 0)
		{
			const Vector3 r = natives::GetGameplayCamRot(2);
			g_walkCam.heading = r.z;
			g_walkCam.pitch = r.x;
			g_walkCam.mode = natives::GetFollowPedCamViewMode();
			const float fov = natives::GetGameplayCamFov();
			g_walkCam.fov = fov >= 30.0f && fov <= 80.0f && g_walkCam.mode != 4 ? fov : 50.0f;
			g_walkCam.cam = natives::CreateCam("DEFAULT_SCRIPTED_CAMERA");
			natives::SetCamActive(g_walkCam.cam, TRUE);
			natives::RenderScriptCams(TRUE);
			g_walkCam.last = now;
			g_walkCam.pull = -1.0f;
		}
		// a mission's own camera ended (GTA went back to its gameplay camera, which is held still here: the view locked):
		// ours renders again. (While a mission's camera renders, it's left alone.)
		if (natives::GetRenderingCam() == -1)
		{
			natives::SetCamActive(g_walkCam.cam, TRUE);
			natives::RenderScriptCams(TRUE);
		}
		const float dt = std::clamp(float(now - g_walkCam.last) * 1e-9f, 0.0f, 0.1f);
		g_walkCam.last = now;
		// a spyglass zooms in (eased, as Minecraft's does), and so does holding Ctrl (OptiFine's zoom: a quarter of the
		// view); the mouse turns slower with it
		// (the mouse wheel zooms further in or out while Ctrl is held; let go, and it's a quarter again next time)
		static float zoom = 1.0f, ctrlZoom = 0.25f;
		const bool ctrl = !screen && !g_mcScoping && natives::IsDisabledControlPressed(0, 36);
		if (ctrl)
		{
			g_ctrlZoomAt = natives::GetGameTimer();
			if (natives::IsDisabledControlJustPressed(0, 15) || natives::IsDisabledControlJustPressed(0, 17))
				ctrlZoom = std::max(0.03f, ctrlZoom * 0.8f);
			if (natives::IsDisabledControlJustPressed(0, 14) || natives::IsDisabledControlJustPressed(0, 16))
				ctrlZoom = std::min(0.8f, ctrlZoom / 0.8f);
		}
		else
			ctrlZoom = 0.25f;
		zoom += ((g_mcScoping ? 0.15f : ctrl ? ctrlZoom : 1.0f) - zoom) * 0.25f;
		for (const int c : {0, 1, 2}) // GTA's own camera stays put (it isn't the one rendering), and V is ours
			natives::DisableControlAction(0, c, TRUE);
		if (!screen)
		{
			float lx, ly;
			if (natives::IsUsingKeyboardAndMouse())
			{
				lx = natives::GetDisabledControlUnboundNormal(0, 1) * kMouseLook;
				ly = natives::GetDisabledControlUnboundNormal(0, 2) * kMouseLook;
			}
			else
			{
				lx = natives::GetDisabledControlNormal(0, 1) * kPadLook * dt;
				ly = natives::GetDisabledControlNormal(0, 2) * kPadLook * dt;
			}
			lx *= g_settings.lookSensitivity * zoom;
			ly *= g_settings.lookSensitivity * zoom * (g_settings.invertLook ? -1.0f : 1.0f);
			g_walkCam.heading = wrap_degrees(g_walkCam.heading - lx);
			g_walkCam.pitch = std::clamp(g_walkCam.pitch - ly, -89.5f, 89.5f);
			if (natives::IsDisabledControlJustPressed(0, 0))
			{
				g_walkCam.mode = g_walkCam.mode == 4 ? 0 : g_walkCam.mode >= 2 ? 4 : g_walkCam.mode + 1;
				natives::SetFollowPedCamViewMode(g_walkCam.mode);
			}
		}
		const float d2r = 3.14159265f / 180.0f, h = g_walkCam.heading * d2r, pt = g_walkCam.pitch * d2r;
		const float fx = -std::sin(h) * std::cos(pt), fy = std::cos(h) * std::cos(pt), fz = std::sin(pt);
		const float ex = g_walk.x, ey = g_walk.y, ez = g_walk.z + 1.62f; // Steve's eyes
		float cx = ex, cy = ey, cz = ez;
		if (g_walkCam.mode != 4)
		{
			const float dist = g_walkCam.mode == 0 ? 2.8f : g_walkCam.mode == 1 ? 4.0f : 5.5f;
			float keep = dist;
			Vector3 at = {}, n = {};
			Entity e = 0;
			// (GTA's surfaces mined out don't stop it: world_probe passes them)
			Hash material = 0;
			if (!g_mcSpectator && world_probe(ped, ex, ey, ez, ex - fx * dist, ey - fy * dist, ez - fz * dist, 1 | 2 | 16, 7, at, n, e, material) &&
				(e == 0 || !g_props.handles.count(e)))
			{
				const float d = std::sqrt((at.x - ex) * (at.x - ex) + (at.y - ey) * (at.y - ey) + (at.z - ez) * (at.z - ez));
				keep = std::max(0.15f, d - 0.3f);
			}
			// nor through Minecraft's blocks: in a tunnel it stays in the tunnel
			if (!g_mcSpectator && !g_props.blocks.empty())
				for (float t = 0.1f; t <= keep; t += 0.1f)
				{
					const float px = ex - fx * t, py = ey - fy * t, pz = ez - fz * t;
					const int bx = int(std::floor(px)), by = int(std::floor(pz + g_yOffset)), bz = int(std::floor(-py));
					if (g_props.blocks.count(std::make_tuple(bx, by, bz)))
					{
						keep = std::max(0.15f, t - 0.25f);
						break;
					}
				}
			// in at once when something comes between (never through it), and eased back out when it's gone (it jumped
			// out in one frame, past a lamp post or a doorway)
			if (g_walkCam.pull < 0.0f || keep < g_walkCam.pull)
				g_walkCam.pull = keep;
			else
				g_walkCam.pull += (keep - g_walkCam.pull) * (1.0f - std::exp(-dt * 4.0f));
			cx = ex - fx * g_walkCam.pull;
			cy = ey - fy * g_walkCam.pull;
			cz = ez - fz * g_walkCam.pull;
		}
		natives::SetCamCoord(g_walkCam.cam, cx, cy, cz);
		natives::SetCamRot(g_walkCam.cam, g_walkCam.pitch, 0.0f, g_walkCam.heading);
		natives::SetCamFov(g_walkCam.cam, (g_walkCam.mode == 4 ? 65.0f : g_walkCam.fov) * zoom);
		// GTA's picture as its own camera gives it: no extra blur or depth of field, and (third person) its near plane
		natives::SetCamNearClip(g_walkCam.cam, g_walkCam.mode == 4 ? 0.05f : 0.15f);
		natives::SetCamMotionBlurStrength(g_walkCam.cam, 0.0f);
		natives::SetCamUseShallowDofMode(g_walkCam.cam, FALSE);
		natives::SetCamDofStrength(g_walkCam.cam, 0.0f);
	}

	/// Steve's pose from GTA's character `who`, in Minecraft's coordinates: the body's frame (up from the pelvis to the
	/// neck, right across the shoulders and hips), the head's (forward and up, from the eyes and the upper lip), where
	/// each arm points (shoulder to wrist) and each leg (hip to ankle). Minecraft turns Steve's body, head, arms and legs
	/// to match, so he moves, sits and lies as the character does. And where his feet go: standing, on GTA's ground
	/// under the hips; sitting or lying (a car seat, a cutscene's sofa), so his hips are where the character's are.
	struct Rig
	{
		float v[24]; // up, right, head forward, head up, left arm, right arm, left leg, right leg
		float feetX, feetY, feetZ; // GTA coordinates
	};
	float g_rigHeadTilt = 0.18f; // radians the face's lip-to-eyes line leans back from the head's up (lips stick out)
	float g_steveHeight = 1.8f; // the character's height (soles to the top of the head): Steve is drawn that tall
	Hash g_steveHeightModel = 0;

	/// Measures the character's height while it stands still and upright (Steve is scaled to it).
	void steve_height_tick(Ped who)
	{
		const Hash model = natives::GetEntityModel(who);
		if (model != g_steveHeightModel)
		{
			g_steveHeightModel = model;
			g_steveHeight = 1.8f;
		}
		if (natives::IsPedInAnyVehicle(who, FALSE) || natives::IsPedRagdoll(who) || natives::IsEntityInAir(who) ||
			natives::IsPedSwimming(who) || natives::GetEntitySpeed(who) > 2.5f)
			return;
		const Vector3 head = natives::GetPedBoneCoords(who, 0x796E), pelvis = natives::GetPedBoneCoords(who, 0x2E28);
		const float foot = std::min(natives::GetPedBoneCoords(who, 0x3779).z, natives::GetPedBoneCoords(who, 0xCC4D).z);
		if (pelvis.z - foot < 0.75f || head.z - pelvis.z < 0.45f || std::fabs(head.x - pelvis.x) + std::fabs(head.y - pelvis.y) > 0.25f)
			return; // crouched, sitting or leaning
		// (the head bone is ~0.13 m under the top of the head, the ankles ~0.09 m over the soles)
		const float h = std::clamp(head.z + 0.13f - (foot - 0.09f), 1.5f, 2.05f);
		g_steveHeight += (h - g_steveHeight) * 0.05f;
	}

	/// Where Steve is on screen this frame, for the compositor: his box (the character's, a little wider: Steve's
	/// blocky), how deep he is along the camera, and glass between him and the camera (a car's window, a shop's).
	void steve_region(Ped ped, const Vector3 &c, const Vector3 &r, float sx, float sy, float sz, bool show, bool seated = false)
	{
		if (!show)
		{
			compositor::set_steve(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1.8f, false);
			return;
		}
		const float d2r = 3.14159265f / 180.0f, ht = g_steveHeight;
		const float fx = -std::sin(r.z * d2r) * std::cos(r.x * d2r), fy = std::cos(r.z * d2r) * std::cos(r.x * d2r), fz = std::sin(r.x * d2r);
		float x0 = 1.0f, y0 = 1.0f, x1 = 0.0f, y1 = 0.0f, dn = 1e9f, df = 0.0f;
		bool behind = false;
		for (int i = 0; i < 8; ++i)
		{
			const float px = sx + ((i & 1) ? 0.75f : -0.75f), py = sy + ((i & 2) ? 0.75f : -0.75f), pz = sz + ((i & 4) ? ht + 0.25f : -0.15f);
			const float d = (px - c.x) * fx + (py - c.y) * fy + (pz - c.z) * fz;
			dn = std::min(dn, d);
			df = std::max(df, d);
			float u = 0.0f, v = 0.0f;
			if (d < 0.05f || !natives::GetScreenCoordFromWorldCoord(px, py, pz, &u, &v))
			{
				behind = true;
				continue;
			}
			x0 = std::min(x0, u);
			y0 = std::min(y0, v);
			x1 = std::max(x1, u);
			y1 = std::max(y1, v);
		}
		if (df <= 0.0f)
		{
			compositor::set_steve(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1.8f, false);
			return;
		}
		if (behind)
			x0 = y0 = 0.0f, x1 = y1 = 1.0f; // around the camera (first person): all of the screen
		// glass in front of him, cell by cell (a 5 x 8 grid on a plane through him, square to the camera: its picture is
		// a box on screen): lines from the camera to him, and where the first thing one meets is glass (a car's window, a
		// shop's), he shows through that glass there. Anything else in front (a wall, a car's body, a pillar) hides him.
		{
			const float rx = std::cos(r.z * d2r), ry = std::sin(r.z * d2r); // (the camera's right)
			const float ux = ry * fz, uy = -rx * fz, uz = rx * fy - ry * fx; // (its up: right x forward)
			const float mx = sx, my = sy, mz = sz + ht * 0.5f, hw = 0.6f, hh = ht * 0.5f + 0.2f;
			float gbox[4] = {0, 0, 0, 0}, cells[40] = {};
			if (!behind && natives::GetScreenCoordFromWorldCoord(mx - rx * hw + ux * hh, my - ry * hw + uy * hh, mz + uz * hh, &gbox[0], &gbox[1]) &&
				natives::GetScreenCoordFromWorldCoord(mx + rx * hw - ux * hh, my + ry * hw - uy * hh, mz - uz * hh, &gbox[2], &gbox[3]))
			{
				for (int j = 0; j < 8; ++j)
					for (int i = 0; i < 5; ++i)
					{
						const float a = -hw + (i + 0.5f) * (2.0f * hw / 5.0f), b = hh - (j + 0.5f) * (2.0f * hh / 8.0f);
						BOOL hit = FALSE;
						Vector3 end = {}, normal = {};
						Entity entity = 0;
						Hash material = 0;
						const int probe = natives::StartShapeTestLosProbe(c.x, c.y, c.z, mx + rx * a + ux * b, my + ry * a + uy * b, mz + uz * b,
							kGlassFlags, ped, kSeeGlass);
						if (natives::GetShapeTestResultIncludingMaterial(probe, &hit, &end, &normal, &material, &entity) == 2 && hit &&
							breakable_glass(material))
							cells[j * 5 + i] = (end.x - c.x) * fx + (end.y - c.y) * fy + (end.z - c.z) * fz;
					}
			}
			else
				gbox[0] = gbox[1] = gbox[2] = gbox[3] = 0.0f;
			compositor::set_steve_glass(gbox, cells);
		}
		compositor::set_steve(std::max(0.0f, x0 - 0.02f), std::max(0.0f, y0 - 0.02f), std::min(1.0f, x1 + 0.02f), std::min(1.0f, y1 + 0.02f),
			std::max(0.0f, dn), df, 0.0f, sx, sz + g_yOffset, -sy, ht, seated);
	}

	bool rig_pose(Ped who, bool seated, Rig &out)
	{
		struct V
		{
			float x, y, z;
		};
		auto bone = [&](int id) {
			const Vector3 b = natives::GetPedBoneCoords(who, id);
			return V{b.x, b.y, b.z};
		};
		auto sub = [](V a, V b) { return V{a.x - b.x, a.y - b.y, a.z - b.z}; };
		auto add = [](V a, V b) { return V{a.x + b.x, a.y + b.y, a.z + b.z}; };
		auto mul = [](V a, float k) { return V{a.x * k, a.y * k, a.z * k}; };
		auto dot = [](V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
		auto len = [&](V a) { return std::sqrt(dot(a, a)); };
		auto cross = [](V a, V b) { return V{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; };
		auto unit = [&](V a, V fallback) {
			const float l = len(a);
			return l > 1e-4f ? mul(a, 1.0f / l) : fallback;
		};
		const V pelvis = bone(0x2E28), neck = bone(0x9995);
		const V lUpper = bone(0xB1C5), rUpper = bone(0x9D4D), lHand = bone(0x49D9), rHand = bone(0xDEAD);
		const V lThigh = bone(0xE39F), rThigh = bone(0xCA72), lFoot = bone(0x3779), rFoot = bone(0xCC4D);
		const V lEye = bone(0x62AC), rEye = bone(0x6B52), lip = bone(0x4ED2);
		const float spine = len(sub(neck, pelvis));
		if (spine < 0.25f || spine > 1.2f)
			return false; // no skeleton there (not streamed in, or not a person)
		const V up = mul(sub(neck, pelvis), 1.0f / spine);
		const V across = add(sub(rUpper, lUpper), sub(rThigh, lThigh));
		const V right = unit(sub(across, mul(up, dot(across, up))), V{1, 0, 0});
		// the head: right across the eyes, up from the upper lip to them (tilted forward a little), forward from those
		V hRight = right, hUp = up;
		const V eyes = sub(rEye, lEye);
		const float eyeGap = len(eyes);
		if (eyeGap > 0.03f && eyeGap < 0.14f)
		{
			hRight = mul(eyes, 1.0f / eyeGap);
			const V face = sub(mul(add(lEye, rEye), 0.5f), lip);
			hUp = unit(sub(face, mul(hRight, dot(face, hRight))), up);
		}
		V hFwd = cross(hUp, hRight);
		const float c = std::cos(g_rigHeadTilt), sn = std::sin(g_rigHeadTilt);
		const V hF = add(mul(hFwd, c), mul(hUp, -sn)), hU = add(mul(hUp, c), mul(hFwd, sn));
		const V lArm = unit(sub(lHand, lUpper), mul(up, -1)), rArm = unit(sub(rHand, rUpper), mul(up, -1));
		const V lLeg = unit(sub(lFoot, lThigh), mul(up, -1)), rLeg = unit(sub(rFoot, rThigh), mul(up, -1));
		const V dirs[8] = {up, right, hF, hU, lArm, rArm, lLeg, rLeg};
		for (int i = 0; i < 8; ++i)
		{
			// GTA (x, y, z) -> Minecraft (x, z, -y)
			out.v[i * 3] = dirs[i].x;
			out.v[i * 3 + 1] = dirs[i].z;
			out.v[i * 3 + 2] = -dirs[i].y;
		}
		// feet: hips on the character's (Steve's hips are 0.375 of his height over his soles), or standing on the ground
		// under them; in between as the legs bend
		const V hipAnchored = sub(pelvis, mul(up, 0.375f * g_steveHeight));
		const float legsDown = (dot(lLeg, mul(up, -1)) + dot(rLeg, mul(up, -1))) * 0.5f;
		const float stand = seated || up.z < 0.75f ? 0.0f : std::clamp((legsDown - 0.55f) / 0.25f, 0.0f, 1.0f);
		const float ground = std::min(lFoot.z, rFoot.z) - 0.09f; // (the ankle bones are that far over the soles)
		out.feetX = pelvis.x + (hipAnchored.x - pelvis.x) * (1.0f - stand);
		out.feetY = pelvis.y + (hipAnchored.y - pelvis.y) * (1.0f - stand);
		out.feetZ = hipAnchored.z + (ground - hipAnchored.z) * stand;
		return true;
	}

	/// Minecraft gets no input from now on (a cutscene, a loading screen, a mission's scene) or again: let go of what
	/// was held (a held attack would go on swinging through the cutscene) and close its screen.
	void input_gate(bool block)
	{
		if (block == g_noInput)
			return;
		g_noInput = block;
		if (!block)
			return;
		for (const char *k : {"attack", "use", "drop"})
			sendf("{\"t\":\"key\",\"k\":\"%s\",\"down\":false}", k);
		if (g_mouseLeft)
			sendf("{\"t\":\"mbtn\",\"b\":1,\"down\":false,\"m\":0}");
		if (g_mouseRight)
			sendf("{\"t\":\"mbtn\",\"b\":3,\"down\":false,\"m\":0}");
		g_mouseLeft = g_mouseRight = false;
		if (g_screen.load() != 0)
			g_ws.send("{\"t\":\"key\",\"k\":\"escape\",\"down\":true}");
		compositor::set_cursor(0.0f, 0.0f, false);
		g_cursorX = g_cursorY = -1.0f;
		g_wantChat = g_wantInventory = false;
		g_spaceDownAt = INT_MAX;
	}

	/// The player became another character (a switch to Michael, Franklin or Trevor, or a mission's): the one left
	/// behind gets back all we did to it (shown, unfrozen, solid, mortal), and the new one gets Steve.
	void character_switched(Ped ped)
	{
		const Ped old = g_lastPed;
		g_lastPed = ped;
		if (old != 0 && natives::DoesEntityExist(old))
		{
			natives::SetEntityVisible(old, TRUE, FALSE);
			if (g_walk.frozen == old || g_drive.on)
			{
				natives::FreezeEntityPosition(old, FALSE);
				natives::SetEntityCollision(old, TRUE, TRUE);
			}
			if (g_godMode)
			{
				natives::SetEntityInvincible(old, FALSE);
				natives::SetEntityProofs(old, FALSE, FALSE, FALSE, FALSE, FALSE);
				natives::SetPedCanRagdoll(old, TRUE);
			}
		}
		if (g_hiddenPed == old)
			g_hiddenPed = 0;
		g_walk.on = false;
		g_walk.acked = false;
		g_walk.frozen = 0;
		walk_cam_end();
		if (g_drive.on)
		{
			natives::RenderScriptCams(FALSE);
			natives::DestroyCam(g_drive.cam);
			g_drive.cam = 0;
			g_drive.on = false;
			g_drive.armed = false;
		}
		if (g_godMode && old != 0)
			make_safe(ped);
	}

	/// MCPassthrough.ini next to MCPassthrough.asi: [Minecraft] LookSensitivity (1.0), InvertLook (0), FreeLook (1).
	void load_settings()
	{
		char path[MAX_PATH] = {};
		const DWORD n = GetModuleFileNameA(g_module, path, MAX_PATH);
		char *dot = n > 0 ? std::strrchr(path, '.') : nullptr;
		if (dot == nullptr || dot + 5 > path + MAX_PATH)
			return;
		std::memcpy(dot, ".ini", 5);
		char value[64] = {};
		GetPrivateProfileStringA("Minecraft", "LookSensitivity", "1.0", value, sizeof(value), path);
		g_settings.lookSensitivity = std::clamp(float(std::atof(value)), 0.05f, 10.0f);
		g_settings.invertLook = GetPrivateProfileIntA("Minecraft", "InvertLook", 0, path) != 0;
		g_settings.freeLook = GetPrivateProfileIntA("Minecraft", "FreeLook", 1, path) != 0;
		g_settings.keepMinimap = GetPrivateProfileIntA("Minecraft", "KeepMinimap", 0, path) != 0;
		g_settings.director = GetPrivateProfileIntA("Minecraft", "Director", 0, path) != 0;
	}

	float g_lastX = 0.0f, g_lastY = 0.0f;

	void tick()
	{
		const Ped ped = natives::PlayerPedId();
		if (ped != g_lastPed)
			character_switched(ped);
		if (g_toggle.exchange(false))
		{
			g_enabled = !g_enabled;
			natives::Notify(g_enabled ? "Minecraft passthrough ~g~on" : "Minecraft passthrough ~r~off");
		}
		if (g_toggleGod.exchange(false))
		{
			g_godMode = !g_godMode;
			apply_rules(ped);
			natives::Notify(g_godMode ? "God mode ~g~on~s~ (can't die, no police)" : "God mode ~r~off~s~ (GTA's own rules)");
		}
		if (g_toggleJump.exchange(false))
		{
			g_mcMove = !g_mcMove;
			natives::Notify(g_mcMove ? "Minecraft movement ~g~on" : "Minecraft movement ~r~off~s~ (GTA's own)");
		}
		const bool on = g_enabled && g_ws.connected();
		const Player player = natives::PlayerId();
		// cutscenes, character switches, death and arrest play out GTA's way (Steve stands in for the player all the same)
		// (a cutscene playing, not one a mission has only loaded ahead: the prologue keeps the next one loaded while it's
		// played, which took Minecraft away for half of it)
		const bool scene = natives::IsCutscenePlaying() || natives::IsPlayerSwitchInProgress() ||
			natives::IsEntityDead(ped) || natives::IsPlayerBeingArrested(player);
		const bool hidden = natives::IsPauseMenuActive() || natives::IsScreenFadedOut();
		// loading screens, fades, warnings and missions that take the player's control: no Minecraft input either
		const bool busy = hidden || natives::GetIsLoadingScreenActive() || natives::IsScreenFadingOut() || natives::IsScreenFadingIn() ||
			natives::IsWarningMessageActive() || !natives::IsPlayerControlOn(player);
		input_gate(!on || scene || busy);
		compositor::set_active(on && !hidden);
		if (g_hiddenPed != 0 && (g_hiddenPed != ped || !on))
			unhide_player(); // a character switch, or the passthrough off: GTA's player shows again
		if (!on)
		{
			if (g_mapLocked)
			{
				natives::UnlockMinimapAngle();
				g_mapLocked = false;
			}
			// an elytra flight or a flying car mid-air: back to GTA's own control (camera, player unfrozen, the car falls)
			if (g_drive.on)
				drive_set(ped, false, true);
			g_drive.armed = false;
			car_fly_end();
			totem_unbuffer();
			walk_set(ped, false);
			walk_cam_end();
			if (!g_props.live.empty())
				props_clear_all();
			if (!g_mobs.empty() || !g_squad.empty())
				mobwar_clear_all();
			compositor::set_screen_fx(0.0f, 0.0f, 0.0f, 0.0f);
			g_screen = 0;
			screen_tick();
			g_wantChat = g_wantInventory = false;
			aim_cam_tick(false);
			gun_model_tick(ped, false);
			leash_clear_all(ped, false);
			if (!g_riders.empty())
				riders_clear_all(g_ws.connected());
			if (!g_scriptFires.empty())
				script_fires_clear();
			std::string ignored;
			while (g_ws.poll(ignored))
			{
			}
			return;
		}

		if (g_ws.generation() != g_generation)
		{
			// (re)connected: size Minecraft's window to GTA's picture and start the ground over
			g_generation = g_ws.generation();
			g_haveOffset = false;
			g_viewSent = 0;
			g_screen = 0;
			g_mcHudHidden = false;
			walk_set(ped, false);
			natives::Notify("Minecraft passthrough ~g~connected");
			logf("connected to Minecraft");
			natives::Notify("~y~E~s~ inventory  ~y~T~s~ chat  ~y~Tab~s~ GTA weapons  ~y~Shift~s~ sprint  ~y~C~s~ sneak  ~y~hold Space~s~ at a high ledge climbs  ~y~F~s~ ladders");
			natives::Notify("~y~F6~s~ Minecraft movement  ~y~F9~s~ god mode  ~y~F7~s~ Minecraft off/on  ~y~F8~s~ fix ground");
			if (g_godMode)
				make_safe(ped);
			mobwar_clear_all();
			g_hot.clear();
			g_hotClusters.clear();
			g_water.clear();
			g_waterClusters.clear();
			g_ws.send("{\"t\":\"nethersync\"}");
		}
		weapon_assets_tick();
		if (natives::GetFrameCount() % 30 == 0)
		{
			if (g_godMode && !g_police)
				natives::ClearPlayerWantedLevel(natives::PlayerId());
			send_state(ped);
		}

		// Minecraft's window = GTA's picture (its real backbuffer, not the monitor), at up to ~1080p worth of
		// pixels (the effect scales it up; at 5120x1440 a full-size copy would be ~90 MB of readback per frame).
		int bw = 0, bh = 0;
		compositor::backbuffer_size(bw, bh);
		if (bw > 0 && bh > 0 && (bw * 65536 + bh) != g_viewSent)
		{
			g_viewSent = bw * 65536 + bh;
			const double scale = std::min(1.0, std::sqrt(kMaxMinecraftPixels / (double(bw) * bh)));
			sendf("{\"t\":\"view\",\"w\":%d,\"h\":%d}", int(bw * scale + 0.5), int(bh * scale + 0.5));
		}

		const Vector3 p = natives::GetEntityCoords(ped, TRUE);
		// moved far in one frame (a respawn, a taxi ride, a mission's teleport): Minecraft's ground starts over here
		const float jx = p.x - g_lastX, jy = p.y - g_lastY;
		if (!g_drive.on && jx * jx + jy * jy > 50.0f * 50.0f)
			g_haveOffset = false;
		g_lastX = p.x;
		g_lastY = p.y;
		const bool relevel = g_relevel.exchange(false);
		if (relevel)
			load_settings(); // (F8 also re-reads MCPassthrough.ini)
		if (!g_haveOffset || relevel)
		{
			float groundZ = 0.0f;
			if (natives::GetGroundZFor3dCoord(p.x, p.y, p.z + 1.0f, &groundZ, FALSE, FALSE))
			{
				g_yOffset = std::round(groundZ) - groundZ;
				g_haveOffset = true;
				g_levelX = p.x;
				g_levelY = p.y;
				logf("ground levelled at %.1f %.1f %.1f (offset %.3f)%s", p.x, p.y, p.z, g_yOffset, relevel ? " (F8)" : "");
				g_sampled.clear();
				g_ws.send("{\"t\":\"clear\"}");
				props_clear_all();
				g_ws.send("{\"t\":\"blocksync\",\"r\":64}");
				if (g_walk.on)
				{
					// the same spot in GTA's world is another height in Minecraft's now
					walk_correct(false, "relevel");
					g_proxy.dirty = true;
				}
			}
		}

		const bool control = !scene && !busy;
		const bool screen = control && screen_tick();
		// Minecraft's movement on foot (GTA's player follows Minecraft's), or GTA's own
		spectator_apply(ped);
		walk_set(ped, control && !hidden && walk_wanted(ped));
		int input = 0;
		float waterTop = 0.0f;
		const bool swimming = g_walk.on && natives::GetWaterHeight(g_walk.x, g_walk.y, g_walk.z + 2.0f, &waterTop) && waterTop > g_walk.z + 0.3f;
		// actually flying (not just /fly allowed), or swimming: Space is up then, no climbing ledges; F's ladders always
		const bool flying = g_mcFlying || g_carFly.on || swimming;
		g_doing = "Minecraft movement";
		if (g_walk.on && !(!screen && !g_mcSpectator && ((!flying && walk_climb(ped)) || walk_ladder(ped))))
			input = walk_tick(ped, screen);
		// a flying car (in the driver's seat, Space twice)
		if (!g_walk.on)
		{
			const int carInput = car_fly_tick(ped, control && !screen);
			if (g_carFly.on)
				input = carInput;
		}
		g_doing = "the rest of the frame";
		stamina_tick(ped, input);
		thieves_tick(ped);
		bow_tick(ped);
		g_doing = "leads";
		leash_tick(ped);
		loose_tick(ped);
		victims_tick();
		later_tick();
		landing_tick(ped);
		g_doing = "boats";
		boats_tick(ped);
		g_doing = "the rest of the frame";
		cheats_tick(ped);
		kill_checks_tick(natives::GetGameTimer());
		arrows_tick();
		// flying (Minecraft's flight, the elytra, a flying car): nothing driving underneath knocks the player down
		{
			static bool noRagdoll = false;
			const bool want = g_drive.on || g_carFly.on || (g_walk.on && (g_mcFlying || g_flyAllowed));
			if (want)
			{
				natives::SetPedCanRagdoll(ped, FALSE);
				natives::SetPedCanRagdollFromPlayerImpact(ped, FALSE);
			}
			else if (noRagdoll)
			{
				natives::SetPedCanRagdollFromPlayerImpact(ped, TRUE);
				natives::SetPedCanRagdoll(ped, !g_godMode);
			}
			noRagdoll = want;
		}
		// Minecraft's free look while Minecraft moves the player
		walk_cam_tick(ped, g_walk.on, screen);
		// the elytra with GTA's own movement (Minecraft's has its own): Space twice in the air, wearing one
		bool elytraTap = false;
		if (control && !screen && !g_walk.on && g_mcElytra && natives::IsDisabledControlJustPressed(0, 22))
		{
			const int now = natives::GetGameTimer();
			elytraTap = now - g_lastSpace < 350 && (natives::IsEntityInAir(ped) || natives::IsPedFalling(ped) || natives::IsPedJumping(ped));
			g_lastSpace = elytraTap ? -100000 : now;
		}
		if (elytraTap)
		{
			g_drive.user = true;
			g_drive.armed = true;
			g_drive.armAfter = 0;
		}
		if (g_drive.on && scene)
			drive_set(ped, false);
		if (control && g_drive.armed && !g_drive.on && (elytraTap || (!g_drive.user && drive_should_launch(ped))))
		{
			// off the edge: Minecraft takes over (elytra, launched along the arm heading/pitch, or where the player looks)
			const Vector3 look = natives::GetGameplayCamRot(2);
			drive_set(ped, true);
			g_drive.heading = g_drive.sHeading = g_drive.user ? look.z : g_drive.armHeading;
			g_drive.pitch = g_drive.sPitch = g_drive.user ? std::clamp(look.x, -80.0f, 70.0f) : g_drive.armPitch;
			g_drive.lookUntil = natives::GetGameTimer() + (g_drive.user ? 0 : g_drive.armHold);
			g_drive.launchedAt = natives::GetGameTimer();
			// the player's own flight uses the elytra worn; the director's puts one on
			sendf("{\"t\":\"glide\",\"on\":true,\"speed\":%.3f,\"equip\":%s}", g_drive.armSpeed, g_drive.user ? "false" : "true");
			sendf("{\"t\":\"gtainfo\",\"event\":\"jump\"}");
		}
		if (g_drive.on)
			drive_tick(ped);

		// The camera GTA rendered with, and where the player stands (Minecraft draws them in third person).
		const Vector3 c = natives::GetFinalRenderedCamCoord();
		const Vector3 r = natives::GetFinalRenderedCamRot(2);
		const float fov = natives::GetFinalRenderedCamFov();
		// the minimap turns with the camera that's rendering (GTA's only follows its own gameplay camera)
		{
			const bool scripted = g_drive.on || g_walkCam.cam != 0;
			if (scripted)
				natives::LockMinimapAngle(int(std::fmod(std::fmod(r.z, 360.0f) + 360.0f, 360.0f)));
			else if (g_mapLocked)
				natives::UnlockMinimapAngle();
			g_mapLocked = scripted;
		}
		compositor::set_host_planes(natives::GetFinalRenderedCamNearClip(), natives::GetFinalRenderedCamFarClip());
		// who Steve stands in for: GTA's player, or in a cutscene its copy of the player's character
		const Ped who = scene ? steve_ped(ped) : ped;
		steve_height_tick(who);
		const bool inVehicle = natives::IsPedInAnyVehicle(who, FALSE) != FALSE;
		const bool firstPerson = !scene && !g_drive.on &&
			(g_walkCam.cam != 0 ? g_walkCam.mode : inVehicle ? natives::GetFollowVehicleCamViewMode() : natives::GetFollowPedCamViewMode()) == 4;
		const bool gun = g_gtaHands && !g_drive.on && !scene;
		const float mcYaw = wrap_degrees(180.0f - r.z), mcPitch = -r.x, mcRoll = r.y;
		compositor::set_host_pose(mcYaw, mcPitch, mcRoll, fov, c.x, c.z + g_yOffset, -c.y);
		compositor::set_camera_locked(g_drive.on);
		if (g_drive.on)
			steve_region(ped, c, r, 0, 0, 0, false); // (the chase cam isn't re-projected)
		if (g_drive.on && g_drive.haveOut)
		{
			// the chase cam as set this frame, and the Steve position it framed: Minecraft draws him exactly there
			const struct { float yaw, pitch; } o = {wrap_degrees(180.0f - g_drive.outHeading), -g_drive.outPitch};
			compositor::set_host_pose(o.yaw, o.pitch, 0.0f, g_drive.outFov, g_drive.outX, g_drive.outZ + g_yOffset, -g_drive.outY);
			sendf("{\"t\":\"cam\",\"f\":%d,\"p\":[%.4f,%.4f,%.4f],\"r\":[%.3f,%.3f,0],\"fov\":%.3f,\"fp\":false,\"drive\":true,"
				  "\"pl\":[%.4f,%.4f,%.4f],\"look\":[%.3f,%.3f],\"ctl\":%s}",
				natives::GetFrameCount(), g_drive.outX, g_drive.outZ + g_yOffset, -g_drive.outY, o.yaw, o.pitch, g_drive.outFov,
				g_drive.steveX, g_drive.steveZ + g_yOffset, -g_drive.steveY, wrap_degrees(180.0f - g_drive.sHeading), -g_drive.sPitch,
				control ? "true" : "false");
		}
		else if (g_drive.on)
			// no position from Minecraft yet: the player is where GTA's is (Minecraft's camera then stays at GTA's)
			sendf("{\"t\":\"cam\",\"f\":%d,\"p\":[%.4f,%.4f,%.4f],\"r\":[%.3f,%.3f,%.3f],\"fov\":%.3f,\"fp\":false,\"drive\":true,"
				  "\"pl\":[%.4f,%.4f,%.4f],\"look\":[%.3f,%.3f],\"ctl\":%s}",
				natives::GetFrameCount(), c.x, c.z + g_yOffset, -c.y, mcYaw, mcPitch, mcRoll, fov,
				p.x, p.z - 1.0f + g_yOffset, -p.y, wrap_degrees(180.0f - g_drive.sHeading), -g_drive.sPitch, control ? "true" : "false");
		else
		{
			// Steve aims where the crosshair is (third person, Minecraft's items), and places blocks against GTA's walls
			float ax = 0.0f, ay = 0.0f, az = 0.0f, bp[8] = {};
			const bool aim = !scene && !firstPerson && !gun;
			if (aim)
				aim_point(ped, c, r, ax, ay, az);
			const bool hasBlock = !scene && !gun && block_point(ped, c, r, bp);
			// where Steve stands: GTA's player's feet (Minecraft's own while it moves the player); sitting (a car, a
			// chair in a cutscene) lower down
			const Vector3 wp = who == ped ? p : natives::GetEntityCoords(who, TRUE);
			float sx = wp.x, sy = wp.y, sz = wp.z - (inVehicle ? 0.8f : 1.0f);
			bool sit = inVehicle;
			if (scene && !inVehicle)
			{
				// a cutscene's animation moves the body, not always the root: go by the hips and feet
				const Vector3 hips = natives::GetPedBoneCoords(who, 0x2E28);
				const float foot = std::min(natives::GetPedBoneCoords(who, 0x3779).z, natives::GetPedBoneCoords(who, 0xCC4D).z);
				if (std::fabs(foot - sz) < 1.5f && std::fabs(hips.z - wp.z) < 1.5f)
				{
					sit = hips.z - foot < 0.55f;
					sz = sit ? hips.z - 0.8f : foot - 0.1f;
				}
			}
			// while GTA moves the character (not Minecraft), Steve's head, body, arms and legs follow its bones: he
			// walks, sits (in the car's seat), climbs, aims and lies as it does
			Rig rig = {};
			const bool rigOn = !g_walk.on && !g_carFly.on && !firstPerson && rig_pose(who, inVehicle, rig);
			if (rigOn)
			{
				sx = rig.feetX;
				sy = rig.feetY;
				sz = rig.feetZ;
			}
			if (g_walk.on || g_carFly.on)
			{
				sx = g_walk.x;
				sy = g_walk.y;
				sz = g_walk.z;
			}
			char rigJson[400] = "";
			if (rigOn)
			{
				int n = snprintf(rigJson, sizeof(rigJson), ",\"rig\":[");
				for (int i = 0; i < 24 && n < int(sizeof(rigJson)) - 12; ++i)
					n += snprintf(rigJson + n, sizeof(rigJson) - n, "%s%.3f", i ? "," : "", rig.v[i]);
				snprintf(rigJson + n, sizeof(rigJson) - n, "]");
			}
			const bool dead = natives::IsEntityDead(ped) || natives::IsPedDeadOrDying(ped);
			// GTA's health, armour and stamina, for Minecraft's hearts, armour and hunger bars
			const int hp = std::max(0, real_health(ped) - 100), hpMax = std::max(1, real_max_health(ped) - 100);
			// GTA's breath under its water while GTA moves the player (Minecraft's air bubbles show it, and Minecraft doesn't
			// drown its player on top of GTA's own drowning); -1 while Minecraft moves it (Minecraft's breath then)
			float air = -1.0f;
			if (!g_walk.on && !g_drive.on)
			{
				static float most = 10.0f;
				static Ped mostOf = 0;
				const float left = natives::GetPlayerUnderwaterTimeRemaining(player);
				if (!natives::IsPedSwimmingUnderWater(ped))
				{
					if (left > 0.5f && (left > most || mostOf != ped))
						most = left; // (a full breath: each character's own lungs)
					mostOf = ped;
					air = 1.0f;
				}
				else
					air = std::clamp(left / std::max(1.0f, most), 0.0f, 1.0f);
			}
			// wet: GTA's rain (a drizzle too), or a puddle under the player's feet: Minecraft's rain for its player (a
			// riptide trident works there)
			float wet = natives::GetRainLevel();
			{
				static int nextPuddle = 0;
				static bool puddle = false;
				const int now = natives::GetGameTimer();
				if (now >= nextPuddle)
				{
					nextPuddle = now + 250;
					const Vector3 feet = g_walk.on ? v3(g_walk.x, g_walk.y, g_walk.z) : v3(p.x, p.y, p.z - 1.0f);
					Vector3 at = {}, n = {};
					Entity e = 0;
					Hash material = 0;
					puddle = world_probe(ped, feet.x, feet.y, feet.z + 0.4f, feet.x, feet.y, feet.z - 0.4f, 1, 7, at, n, e, material) &&
						material == kPuddleMaterial;
				}
				if (puddle)
					wet = 1.0f;
			}
			// GTA's phone out: Minecraft's hand and HUD make way for it (they're drawn over everything, the phone too)
			const bool phone = natives::IsPedRunningMobilePhoneTask(ped) != FALSE;
			char bars[200];
			snprintf(bars, sizeof(bars), ",\"hp\":[%d,%d],\"ar\":%d,\"st\":%.1f,\"fly\":%s,\"ht\":%.3f,\"rain\":%.2f,\"air\":%.3f,\"ph\":%s", hp, hpMax,
				natives::GetPedArmour(ped), g_stamina, g_carFly.on ? "true" : "false", g_steveHeight, wet, air, phone ? "true" : "false");
			sendf("{\"t\":\"cam\",\"f\":%d,\"p\":[%.4f,%.4f,%.4f],\"r\":[%.3f,%.3f,%.3f],\"fov\":%.3f,\"fp\":%s,\"pl\":[%.4f,%.4f,%.4f],\"h\":%.3f,"
				  "\"gun\":%s,\"veh\":%s,\"sn\":%s,\"aim\":[%.3f,%.3f,%.3f],\"aimOn\":%s,\"walk\":%s,\"in\":%d,\"dead\":%s,"
				  "\"gh\":[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.0f,%.0f],\"ghOn\":%s,\"ctl\":%s%s%s}",
				natives::GetFrameCount(), c.x, c.z + g_yOffset, -c.y, mcYaw, mcPitch, mcRoll, fov, firstPerson ? "true" : "false", sx,
				sz + g_yOffset, -sy, wrap_degrees(180.0f - natives::GetEntityHeading(who)), holding_gta_weapon(ped) && !scene ? "true" : "false",
				sit && !g_carFly.on ? "true" : "false", !scene && natives::GetPedStealthMovement(ped) ? "true" : "false", ax, ay, az, aim ? "true" : "false",
				g_walk.on || g_carFly.on ? "true" : "false", input, dead ? "true" : "false", bp[0], bp[1], bp[2], bp[3], bp[4], bp[5], bp[6], bp[7], hasBlock ? "true" : "false",
				control ? "true" : "false", rigJson, bars);
			// (first person: Minecraft puts its player's eyes at the camera, so its feet are 1.62 m under it)
			steve_region(ped, c, r, sx, sy, sz, !firstPerson, sit && !g_carFly.on); // (first person: Steve isn't drawn)
		}

		// no camera motion blur (explosions smear GTA's picture, Minecraft's stays sharp: the two look apart)
		natives::SetGameplayCamMotionBlurScalingThisUpdate(0.0f);
		natives::SetGameplayCamMaxMotionBlurStrengthThisUpdate(0.0f);
		// GTA's idle cinematic camera (30 s without input) never cuts in
		natives::InvalidateIdleCam();
		natives::InvalidateCinematicVehicleIdleMode();
		hud_tick(scene);
		// Minecraft draws the player (Steve), the hand and its HUD; in cutscenes Steve stands in for the player's
		// character too (and Minecraft's HUD hides)
		hide_player(ped);
		timescale_tick(ped);
		compositor::set_cutscene(natives::IsCutscenePlaying() != FALSE);
		mission_restart_tick(ped);
		if (scene)
			natives::SetEntityLocallyInvisible(ped);
		if (who != ped)
			natives::SetEntityLocallyInvisible(who);
		if (!control != g_mcHudHidden)
		{
			g_mcHudHidden = !control;
			sendf("{\"t\":\"hud\",\"hidden\":%s}", g_mcHudHidden ? "true" : "false");
		}
		if (control)
		{
			// Tab: GTA's weapons or Minecraft's items (not while flying)
			if (!screen && !g_drive.on && natives::IsDisabledControlJustPressed(0, 37))
				hands_set(ped, !g_gtaHands);
			// a mission's people react only to GTA's own aiming (the prologue's hostages): Minecraft's bow drawn in a
			// mission aims GTA's player's gun along with it, unseen (Steve's bow is what shows; it never fires)
			const Hash missionGun = !gun && g_bowDrawn && natives::GetMissionFlag()
				? (g_gtaWeapon != 0 && natives::HasPedGotWeapon(ped, g_gtaWeapon) ? g_gtaWeapon : natives::GetBestPedWeapon(ped))
				: kWeaponUnarmed;
			if (missionGun != kWeaponUnarmed && missionGun != 0)
			{
				if (natives::GetSelectedPedWeapon(ped) != missionGun)
					natives::SetCurrentPedWeapon(ped, missionGun, TRUE);
				natives::SetPedCurrentWeaponVisible(ped, FALSE);
				g_missionAim = true;
			}
			else if (!gun)
			{
				// Minecraft's items in hand: a weapon a mission gives GTA's player waits for Tab
				g_missionAim = false;
				const Hash w = natives::GetSelectedPedWeapon(ped);
				if (w != kWeaponUnarmed)
				{
					g_gtaWeapon = w;
					natives::SetCurrentPedWeapon(ped, kWeaponUnarmed, TRUE);
				}
			}
			else
				g_missionAim = false;
			if (!screen)
			{
				if (gun)
				{
					// GTA aims and fires with all its weapon keys; Tab switches hands, it doesn't open the weapon wheel.
					// GTA's own weapon fires (flash, tracers); the copy in Steve's hands is what shows; the crosshair is ours
					natives::DisableControlAction(0, 37, TRUE);
					natives::SetPedCurrentWeaponVisible(ped, TRUE);
					if (const Entity w = natives::GetCurrentPedWeaponEntityIndex(ped))
						natives::SetEntityVisible(w, TRUE, FALSE);
					reticle_tick(ped);
				}
				else
				{
					// a car, helicopter or jet with guns or rockets of its own (a mission's Buzzard, a tank): its weapon
					// keys are GTA's, and the mouse fires them, not Minecraft's item
					const bool armed = inVehicle && natives::DoesVehicleHaveWeapons(natives::GetVehiclePedIsIn(ped, FALSE));
					// Q: GTA's cover, always (Minecraft's drop is off)
					const bool cover = !inVehicle;
					static const std::set<int> vehicleWeapons = {68, 69, 70, 91, 92, 99, 100, 114, 115, 116};
					for (int control : kDisabledControls)
						if ((!g_missionAim || control != 25) && // (aiming along with the bow in a mission)
							!(armed && vehicleWeapons.count(control)) && !(cover && control == 44))
							natives::DisableControlAction(0, control, TRUE);
					if (g_missionAim)
						natives::SetControlValueNextFrame(0, 25, 1.0f);
					static bool wasArmed = false;
					if (armed && !wasArmed)
						for (const char *k : {"attack", "use"}) // (nothing left held in Minecraft)
							sendf("{\"t\":\"key\",\"k\":\"%s\",\"down\":false}", k);
					wasArmed = armed;
					// (GTA's phone out: the clicks and the wheel are the phone's)
					const bool phone = natives::IsPedRunningMobilePhoneTask(ped) != FALSE;
					if (!armed && !phone)
					{
						forward_button(24, "attack");
						forward_button(25, "use", g_missionAim ? VK_RBUTTON : 0);
					}
					// (zooming with Ctrl: the wheel is the zoom's)
					const bool zooming = natives::GetGameTimer() - g_ctrlZoomAt < 150;
					if (!phone && !zooming && (natives::IsDisabledControlJustPressed(0, 14) || natives::IsDisabledControlJustPressed(0, 16)))
						g_ws.send("{\"t\":\"scroll\",\"d\":-1}");
					if (!phone && !zooming && (natives::IsDisabledControlJustPressed(0, 15) || natives::IsDisabledControlJustPressed(0, 17)))
						g_ws.send("{\"t\":\"scroll\",\"d\":1}");
					for (int i = 0; i < 9; ++i)
						if (natives::IsDisabledControlJustPressed(0, kHotbarControls[i]))
							sendf("{\"t\":\"slot\",\"n\":%d}", i);
					// E: Minecraft's inventory, unless GTA wants E right now (its help text asks for it)
					if (!inVehicle && !natives::IsHelpMessageBeingDisplayed())
					{
						for (const int e : {38, 46, 51})
							natives::DisableControlAction(0, e, TRUE);
						if (natives::IsDisabledControlJustPressed(0, 51))
							g_wantInventory = true;
					}
				}
				// E or I: Minecraft's inventory, T: its chat (either hands)
				if (g_wantInventory.exchange(false))
				{
					g_ws.send("{\"t\":\"key\",\"k\":\"inventory\",\"down\":true}");
					g_ws.send("{\"t\":\"key\",\"k\":\"inventory\",\"down\":false}");
				}
				if (g_wantChat.exchange(false))
				{
					g_ws.send("{\"t\":\"key\",\"k\":\"chat\",\"down\":true}");
					g_ws.send("{\"t\":\"key\",\"k\":\"chat\",\"down\":false}");
				}
			}
			aim_cam_tick(gun && !screen && natives::IsPlayerFreeAiming(natives::PlayerId()));
			gun_model_tick(ped, gun && !natives::IsPauseMenuActive());
		}
		else
		{
			g_wantChat = g_wantInventory = false;
			aim_cam_tick(false);
			gun_model_tick(ped, false);
		}

		screen_fx_tick();
		crash_tick(ped);
		world_mobs_tick(ped);
		fires_tick(ped);
		animals_tick();
		mobs_tick(ped);
		hell_tick(ped);
		end_tick();
		hot_tick(ped);
		water_tick(ped);
		hell_lights(ped);
		if (g_haveOffset)
		{
			if (!g_drive.on)
				maybe_relevel(p);
			sample_ground(p);
			if (!g_mobs.empty())
				sample_patches();
			props_tick(p);
		}

		std::string message;
		while (g_ws.poll(message))
			handle_event(message);
	}

#ifdef _MSC_VER
	DWORD g_crashCode = 0;
	void *g_crashAt = nullptr;

	int crash_filter(EXCEPTION_POINTERS *e)
	{
		g_crashCode = e->ExceptionRecord->ExceptionCode;
		g_crashAt = e->ExceptionRecord->ExceptionAddress;
		return EXCEPTION_EXECUTE_HANDLER;
	}

	/// A frame, with a failure inside GTA (a native given an entity in a state it didn't expect) caught: the frame is
	/// dropped and the plugin carries on, rather than ScriptHookV stopping it for good with an error box.
	bool guarded_tick()
	{
		__try
		{
			tick();
			return true;
		}
		__except (crash_filter(GetExceptionInformation()))
		{
			return false;
		}
	}
#else
	DWORD g_crashCode = 0;
	void *g_crashAt = nullptr;
	bool guarded_tick()
	{
		tick();
		return true;
	}
#endif

	void script_main()
	{
		if (!g_started)
		{
			g_started = true;
			load_settings();
			g_ws.start("127.0.0.1", kPort);
		}
		int crashes = 0, notedAt = -100000;
		while (true)
		{
			compositor::try_register(g_module);
			g_doing = "the frame";
			if (!guarded_tick())
			{
				// (logged with where in GTA it failed, and what the plugin was doing; told on screen now and then)
				const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)), at = reinterpret_cast<uintptr_t>(g_crashAt);
				if (++crashes <= 50)
					logf("ERROR %08X at GTA5.exe+%llX while doing %s: that frame was skipped", unsigned(g_crashCode),
						(unsigned long long)(at - base), g_doing);
				const int now = natives::GetGameTimer();
				if (now - notedAt > 10000)
				{
					notedAt = now;
					char note[160];
					snprintf(note, sizeof(note), "~r~Minecraft passthrough~s~: GTA failed during %s (skipped; see MCPassthrough.log)", g_doing);
					natives::Notify(note);
				}
			}
			WAIT(0);
		}
	}

	void on_keyboard(DWORD key, WORD, BYTE scanCode, BOOL isExtended, BOOL, BOOL wasDownBefore, BOOL isUpNow)
	{
		// Minecraft's screens (chat, inventory) take the keyboard, except the F keys
		if (g_screen.load() != 0 && !(key >= VK_F1 && key <= VK_F24))
		{
			screen_key(key, scanCode, isExtended, wasDownBefore, isUpNow);
			return;
		}
		if (isUpNow || wasDownBefore)
			return;
		switch (key)
		{
		case VK_F6: g_toggleJump = true; break;
		case VK_F7: g_toggle = true; break;
		case VK_F8: g_relevel = true; break;
		case VK_F9: g_toggleGod = true; break;
		case 'I': g_wantInventory = true; break;
		case 'T': g_wantChat = true; break;
		default: break;
		}
	}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		g_module = module;
		scriptRegister(module, script_main);
		keyboardHandlerRegister(on_keyboard);
		break;
	case DLL_PROCESS_DETACH:
		compositor::unregister(module);
		scriptUnregister(module);
		keyboardHandlerUnregister(on_keyboard);
		break;
	}
	return TRUE;
}
