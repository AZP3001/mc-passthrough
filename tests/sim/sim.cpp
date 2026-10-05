// A GTA stand-in for the script's Minecraft-movement logic: natives answered from a little world of boxes (a floor,
// walls, a ledge), the WebSocket replaced by queues, and a crude Minecraft on the other end (it walks through walls,
// so GTA's own checks have to stop it). Asserts what GTA's side does.
#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <array>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../gta/src/script.cpp"
#include "hashes.h"

int sim_sscanf_s(const char *str, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	void *p[12] = {};
	int n = 0;
	for (const char *f = fmt; *f; ++f)
	{
		if (*f != '%')
			continue;
		++f;
		if (*f == '%' || *f == '*')
			continue;
		while (isdigit((unsigned char)*f))
			++f;
		while (*f == 'l' || *f == 'h')
			++f;
		p[n++] = va_arg(ap, void *);
		if (*f == '[')
		{
			(void)va_arg(ap, unsigned);
			++f;
			if (*f == '^')
				++f;
			if (*f == ']')
				++f;
			while (*f && *f != ']')
				++f;
		}
		else if (*f == 's' || *f == 'c')
			(void)va_arg(ap, unsigned);
	}
	va_end(ap);
	return sscanf(str, fmt, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11]);
}

// ---- the world ----
struct Box
{
	float x0, y0, z0, x1, y1, z1;
	int entity; // 0 map
};
std::vector<Box> g_world;
std::map<int, int> g_types = {{1, 1}}; // entity -> type (1 ped, 2 vehicle, 3 object)
struct SimPed
{
	float x = 0, y = 0, z = 11.4f, heading = 0;
	bool frozen = false;
} g_sped;
int64_t g_nanos = 1000000000LL;
int g_ms = 10000, g_frame = 0;
float g_camHeading = 0, g_camPitch = 0;
bool g_cutscene = false;
int g_playerPed = 1;                 // PLAYER_PED_ID (a character switch changes it)
std::map<int, bool> g_visible;       // SET_ENTITY_VISIBLE
std::map<int, bool> g_frozenOf;      // FREEZE_ENTITY_POSITION, any entity
bool g_kbm = false;                  // IS_USING_KEYBOARD_AND_MOUSE
std::map<int, float> g_look;         // GET_DISABLED_CONTROL_UNBOUND_NORMAL by control
float g_mask[4] = {};                // the compositor's minimap mask
// a standing person's bones (facing north, GTA's +y), relative to the root (1 m over the feet)
const std::map<int, std::array<float, 3>> kBones = {
	{0x2E28, {0.0f, 0.0f, 0.0f}},       // pelvis
	{0x9995, {0.0f, 0.02f, 0.55f}},     // neck
	{0x796E, {0.0f, 0.03f, 0.62f}},     // head
	{0xB1C5, {-0.18f, 0.0f, 0.45f}},    // left upper arm
	{0x9D4D, {0.18f, 0.0f, 0.45f}},     // right upper arm
	{0x49D9, {-0.22f, 0.0f, -0.12f}},   // left hand
	{0xDEAD, {0.22f, 0.0f, -0.12f}},    // right hand
	{0xE39F, {-0.1f, 0.0f, -0.05f}},    // left thigh
	{0xCA72, {0.1f, 0.0f, -0.05f}},     // right thigh
	{0x3779, {-0.1f, 0.0f, -0.91f}},    // left foot
	{0xCC4D, {0.1f, 0.0f, -0.91f}},     // right foot
	{0x62AC, {-0.032f, 0.09f, 0.74f}},  // left eye
	{0x6B52, {0.032f, 0.09f, 0.74f}},   // right eye
	{0x4ED2, {0.0f, 0.1f, 0.68f}},      // upper lip (straight below the eyes: the head looks level after its tilt)
};
// GTA people besides the player (worldGetAllPeds lists them)
struct SimNpc
{
	float x = 0, y = 0, z = 11.4f, vx = 0, vy = 0, vz = 0;
	Hash model = 0;
	bool human = true, frozen = false, inCar = false;
};
std::map<int, SimNpc> g_npcs;
struct SimCar
{
	bool on = false;
	float x = 0, y = 0, z = 11, vx = 0, vy = 0;
} g_car;
// GTA's objects (worldGetAllObjects lists them; a street has hundreds), where they are, and which are pickups
std::vector<int> g_objects;
std::map<int, std::array<float, 3>> g_objPos;
std::set<int> g_pickups;
std::set<int> g_pressed, g_justPressed;
std::vector<std::string> g_calls; // natives of interest called
std::vector<std::string> g_out;   // sent to Minecraft
std::deque<std::string> g_in;     // from Minecraft

BOOL QueryPerformanceCounter(LARGE_INTEGER *c)
{
	c->QuadPart = g_nanos;
	return TRUE;
}

// ray vs boxes: the nearest hit, its normal and entity
struct Hit
{
	bool hit = false;
	float x, y, z, nx, ny, nz;
	int entity;
} g_lastHit;

Hit ray(float x0, float y0, float z0, float x1, float y1, float z1, int flags, int ignore)
{
	Hit best;
	float bestT = 2.0f;
	const float d[3] = {x1 - x0, y1 - y0, z1 - z0}, o[3] = {x0, y0, z0};
	for (const Box &b : g_world)
	{
		if (b.entity == ignore && ignore != 0)
			continue;
		const int type = b.entity == 0 ? 0 : g_types[b.entity];
		if ((type == 0 && !(flags & 1)) || (type == 1 && !(flags & 4)) || (type == 2 && !(flags & 2)) || (type == 3 && !(flags & 16)))
			continue;
		const float lo[3] = {b.x0, b.y0, b.z0}, hi[3] = {b.x1, b.y1, b.z1};
		float tmin = 0.0f, tmax = 1.0f;
		int axis = -1;
		float sign = 0;
		bool inside = true, miss = false;
		for (int a = 0; a < 3 && !miss; ++a)
		{
			if (std::fabs(d[a]) < 1e-9f)
			{
				if (o[a] < lo[a] || o[a] > hi[a])
					miss = true;
				continue;
			}
			float t0 = (lo[a] - o[a]) / d[a], t1 = (hi[a] - o[a]) / d[a];
			float s = -1;
			if (t0 > t1)
			{
				std::swap(t0, t1);
				s = 1;
			}
			if (o[a] < lo[a] || o[a] > hi[a])
				inside = false;
			if (t0 > tmin)
			{
				tmin = t0;
				axis = a;
				sign = s;
			}
			tmax = std::min(tmax, t1);
			if (tmin > tmax)
				miss = true;
		}
		if (miss || inside || axis < 0 || tmin >= bestT)
			continue; // (a ray starting inside a box sees out through its back faces: no hit, like GTA)
		bestT = tmin;
		best.hit = true;
		best.x = x0 + d[0] * tmin;
		best.y = y0 + d[1] * tmin;
		best.z = z0 + d[2] * tmin;
		best.nx = axis == 0 ? sign : 0;
		best.ny = axis == 1 ? sign : 0;
		best.nz = axis == 2 ? sign : 0;
		best.entity = b.entity;
	}
	return best;
}

// ---- ScriptHookV ----
static UINT64 g_args[32];
static int g_argc;
static UINT64 g_hash;
alignas(16) static UINT64 g_ret[4];

void nativeInit(UINT64 hash)
{
	g_hash = hash;
	g_argc = 0;
}
void nativePush64(UINT64 v) { g_args[g_argc++] = v; }

static float F(int i)
{
	float f;
	std::memcpy(&f, &g_args[i], 4);
	return f;
}
static int I(int i) { return int(g_args[i]); }
template <typename T> static T *P(int i) { return reinterpret_cast<T *>(g_args[i]); }
static void retV(float x, float y, float z)
{
	Vector3 v = {};
	v.x = x;
	v.y = y;
	v.z = z;
	std::memcpy(g_ret, &v, sizeof(v));
}
static void retI(int v)
{
	g_ret[0] = 0;
	std::memcpy(g_ret, &v, 4);
}
static void retF(float v)
{
	g_ret[0] = 0;
	std::memcpy(g_ret, &v, 4);
}

PUINT64 nativeCall()
{
	std::memset(g_ret, 0, sizeof(g_ret));
	const UINT64 h = g_hash;
	const auto npc = g_npcs.find(g_argc > 0 ? I(0) : -1);
	if (h == H_PlayerPedId)
		retI(g_playerPed);
	else if (npc != g_npcs.end() && h == H_GetEntityCoords)
		retV(npc->second.x, npc->second.y, npc->second.z);
	else if (npc != g_npcs.end() && h == H_GetEntityVelocity)
		retV(npc->second.vx, npc->second.vy, npc->second.vz);
	else if (npc != g_npcs.end() && h == H_SetEntityCoordsNoOffset)
	{
		npc->second.x = F(1);
		npc->second.y = F(2);
		npc->second.z = F(3);
	}
	else if (npc != g_npcs.end() && h == H_SetEntityVelocity)
	{
		npc->second.vx = F(1);
		npc->second.vy = F(2);
		npc->second.vz = F(3);
	}
	else if (npc != g_npcs.end() && h == H_FreezeEntityPosition)
		npc->second.frozen = I(1) != 0;
	else if (npc != g_npcs.end() && h == H_GetPedBoneCoords)
	{
		const auto b = kBones.find(I(1));
		const float o[3] = {b != kBones.end() ? b->second[0] : 0.0f, b != kBones.end() ? b->second[1] : 0.0f, b != kBones.end() ? b->second[2] : 0.0f};
		retV(npc->second.x + o[0], npc->second.y + o[1], npc->second.z + o[2]);
	}
	else if (npc != g_npcs.end() && h == H_GetEntityModel)
		retI(int(npc->second.model));
	else if (npc != g_npcs.end() && h == H_IsPedHuman)
		retI(npc->second.human);
	else if (npc != g_npcs.end() && h == H_GetOffsetFromEntityGivenWorldCoords)
		retV(F(1) - npc->second.x, F(2) - npc->second.y, F(3) - npc->second.z);
	else if (npc != g_npcs.end() && h == H_TaskGoStraightToCoord)
		g_calls.push_back("Walk:" + std::to_string(I(0)));
	else if (npc != g_npcs.end() && h == H_TaskPlayAnimLoop)
		g_calls.push_back("Sit:" + std::to_string(I(0)));
	else if (npc != g_npcs.end() && h == H_IsPedInAnyVehicle)
		retI(npc->second.inCar);
	else if (npc != g_npcs.end() && h == H_ClearPedTasksImmediately)
	{
		npc->second.inCar = false;
		g_calls.push_back("Out:" + std::to_string(I(0)));
	}
	else if (g_objPos.count(g_argc > 0 ? I(0) : -1) && h == H_GetEntityCoords)
	{
		const auto &o = g_objPos[I(0)];
		retV(o[0], o[1], o[2]);
	}
	else if (h == H_IsObjectAPickup)
		retI(g_pickups.count(I(0)) ? 1 : 0);
	else if (npc == g_npcs.end() && h == H_SetEntityVelocity && I(0) != 1)
		g_calls.push_back("Velocity:" + std::to_string(I(0)));
	else if (h == H_GetModelDimensions)
	{
		Vector3 *mn = P<Vector3>(1), *mx = P<Vector3>(2);
		mn->x = -0.3f;
		mn->y = -0.3f;
		mn->z = -1.0f;
		mx->x = 0.3f;
		mx->y = 0.3f;
		mx->z = 0.8f;
	}
	else if (h == H_HasAnimDictLoaded)
		retI(1);
	else if (h == H_GetEntityCoords)
	{
		if (I(0) == 1)
			retV(g_sped.x, g_sped.y, g_sped.z);
		else if (I(0) == 70)
			retV(g_car.x, g_car.y, g_car.z);
	}
	else if (h == H_GetEntityVelocity)
	{
		if (I(0) == 70)
			retV(g_car.vx, g_car.vy, 0);
	}
	else if (h == H_SetEntityCoordsNoOffset)
	{
		if (I(0) == 1)
		{
			g_sped.x = F(1);
			g_sped.y = F(2);
			g_sped.z = F(3);
		}
	}
	else if (h == H_FreezeEntityPosition)
	{
		if (I(0) == 1)
			g_sped.frozen = I(1) != 0;
		g_frozenOf[I(0)] = I(1) != 0;
	}
	else if (h == H_SetEntityVisible)
		g_visible[I(0)] = I(1) != 0;
	else if (h == H_CreateCam)
		retI(77);
	else if (h == H_DoesEntityExist)
		retI(I(0) != 0);
	else if (h == H_SetCamRot)
	{
		// the free-look camera: what renders (the simulator has one camera for both)
		g_camPitch = F(1);
		g_camHeading = F(3);
	}
	else if (h == H_IsUsingKeyboardAndMouse)
		retI(g_kbm);
	else if (h == H_GetDisabledControlUnboundNormal)
		retF(g_look[I(1)]);
	else if (h == H_IsMinimapRendering)
		retI(1);
	else if (h == H_GetPedBoneCoords)
	{
		const auto b = kBones.find(I(1));
		if (b != kBones.end())
			retV(g_sped.x + b->second[0], g_sped.y + b->second[1], g_sped.z + b->second[2]);
		else
			retV(g_sped.x, g_sped.y, g_sped.z);
	}
	else if (h == H_GetEntityBoneIndexByName)
		retI(I(0) == 70 && std::string(P<const char>(1)) == "wheel_lf" ? 3 : -1);
	else if (h == H_GetWorldPositionOfEntityBone)
		retV(g_car.x - 0.8f, g_car.y + 1.3f, g_car.z - 0.5f); // (wheel_lf)
	else if (h == H_GetVehicleTyresCanBurst)
		retI(1);
	else if (h == H_SetVehicleTyreBurst)
		g_calls.push_back("TyreBurst:" + std::to_string(I(1)));
	else if (h == H_SetEntityHeading)
	{
		if (I(0) == 1)
			g_sped.heading = F(1);
	}
	else if (h == H_GetEntityHeading)
		retF(g_sped.heading);
	else if (h == H_GetGameTimer)
		retI(g_ms);
	else if (h == H_GetFrameCount)
		retI(g_frame);
	else if (h == H_StartShapeTestLosProbe)
	{
		g_lastHit = ray(F(0), F(1), F(2), F(3), F(4), F(5), I(6), I(7));
		retI(7);
	}
	else if (h == H_GetShapeTestResult || h == H_GetShapeTestResultIncludingMaterial)
	{
		const bool mat = h == H_GetShapeTestResultIncludingMaterial;
		*P<BOOL>(1) = g_lastHit.hit;
		Vector3 *end = P<Vector3>(2), *n = P<Vector3>(3);
		end->x = g_lastHit.x;
		end->y = g_lastHit.y;
		end->z = g_lastHit.z;
		n->x = g_lastHit.nx;
		n->y = g_lastHit.ny;
		n->z = g_lastHit.nz;
		if (mat)
		{
			*P<Hash>(4) = 0;
			*P<Entity>(5) = g_lastHit.entity;
		}
		else
			*P<Entity>(4) = g_lastHit.entity;
		retI(2);
	}
	else if (h == H_GetGroundZFor3dCoord)
	{
		const Hit hit = ray(F(0), F(1), F(2), F(0), F(1), F(2) - 200.0f, 1 | 16, 0);
		if (hit.hit)
			*P<float>(3) = hit.z;
		retI(hit.hit);
	}
	else if (h == H_IsPlayerControlOn || h == H_IsPedOnFoot || h == H_IsEntityVisible)
		retI(1);
	else if (h == H_GetPedParachuteState)
		retI(-1);
	else if (h == H_GetGameplayCamRot || h == H_GetFinalRenderedCamRot)
		retV(g_camPitch, 0, g_camHeading);
	else if (h == H_GetGameplayCamCoord || h == H_GetFinalRenderedCamCoord)
	{
		const float r = g_camHeading * 3.14159265f / 180.0f;
		retV(g_sped.x + std::sin(r) * 3.5f, g_sped.y - std::cos(r) * 3.5f, g_sped.z + 0.8f);
	}
	else if (h == H_GetFinalRenderedCamFov || h == H_GetGameplayCamFov)
		retF(50.0f);
	else if (h == H_GetFollowPedCamViewMode)
		retI(1);
	else if (h == H_IsDisabledControlPressed)
		retI(g_pressed.count(I(1)) ? 1 : 0);
	else if (h == H_IsDisabledControlJustPressed)
		retI(g_justPressed.count(I(1)) ? 1 : 0);
	else if (h == H_GetEntityType)
		retI(I(0) == 0 ? 0 : g_types[I(0)]);
	else if (h == H_IsCutsceneActive)
		retI(g_cutscene);
	else if (h == H_GetSelectedPedWeapon)
		retI(int(0xA2719263u));
	else if (h == H_GetSafeZoneSize)
		retF(1.0f);
	else if (h == H_GetAspectRatio)
		retF(1.7777f);
	else if (h == H_TaskClimb)
		g_calls.push_back("TaskClimb");
	else if (h == H_TaskClimbLadder)
		g_calls.push_back("TaskClimbLadder");
	else if (h == H_ApplyForceToEntity)
		g_calls.push_back("ApplyForce:" + std::to_string(I(0)));
	else if (h == H_StartEntityFire)
		g_calls.push_back("Fire:" + std::to_string(I(0)));
	else if (h == H_StartScriptFire)
	{
		g_calls.push_back("ScriptFire");
		retI(900 + int(g_calls.size()));
	}
	else if (h == H_RemoveScriptFire)
		g_calls.push_back("ScriptFireOut");
	else if (h == H_GetHashKey)
		retI(int(std::hash<std::string>{}(P<const char>(0))));
	return g_ret;
}

void scriptWait(DWORD) {}
void scriptRegister(HMODULE, void (*)()) {}
void scriptUnregister(HMODULE) {}
void keyboardHandlerRegister(KeyboardHandler) {}
void keyboardHandlerUnregister(KeyboardHandler) {}
int worldGetAllVehicles(int *arr, int)
{
	if (!g_car.on)
		return 0;
	arr[0] = 70;
	return 1;
}
int worldGetAllPeds(int *arr, int max)
{
	int n = 0;
	for (const auto &[id, p] : g_npcs)
		if (n < max)
			arr[n++] = id;
	return n;
}
int worldGetAllObjects(int *arr, int max)
{
	int n = 0;
	for (const int o : g_objects)
		if (n < max)
			arr[n++] = o;
	return n;
}

// ---- the link ----
void WsClient::start(const char *, int)
{
	m_connected = true;
	m_generation = 1;
}
void WsClient::stop() {}
bool WsClient::send(const std::string &text)
{
	g_out.push_back(text);
	return true;
}
bool WsClient::poll(std::string &message)
{
	if (g_in.empty())
		return false;
	message = g_in.front();
	g_in.pop_front();
	return true;
}

// ---- the compositor ----
namespace compositor
{
	bool try_register(void *) { return true; }
	void unregister(void *) {}
	void set_active(bool) {}
	void set_hud_mask(float x0, float y0, float x1, float y1)
	{
		g_mask[0] = x0;
		g_mask[1] = y0;
		g_mask[2] = x1;
		g_mask[3] = y1;
	}
	void set_cursor(float, float, bool) {}
	void set_host_planes(float, float) {}
	void set_host_pose(float, float, float, float, double, double, double) {}
	void set_pose_lag(int) {}
	void set_steve(float, float, float, float, float, float, float, double, double, double, float, bool) {}
	void set_camera_locked(bool) {}
	void set_look(float, float, float) {}
	void set_screen_fx(float, float, float, float) {}
	int g_digCells = -1; // set_dig: how many cells it was given (-1 never, 0 none)
	void set_dig(int, int, int, const unsigned char *cells)
	{
		g_digCells = 0;
		if (cells != nullptr)
			for (int i = 0; i < 512 * 512; ++i)
				g_digCells += cells[i] != 0;
	}
	void backbuffer_size(int &w, int &h)
	{
		w = 1920;
		h = 1080;
	}
}

// ---- a crude Minecraft: walks where it's told (through walls), on the floor; applies GTA's corrections ----
struct Mc
{
	bool walk = false;
	bool falling = false;
	int input = 0, ps = -1;
	float yaw = 0;
	double x = 0, y = 0, z = 0;
	bool placed = false;
} g_mc;

static double num_after(const std::string &m, const char *key, double fb = 0)
{
	const size_t at = m.find(std::string("\"") + key + "\":");
	return at == std::string::npos ? fb : std::atof(m.c_str() + at + std::strlen(key) + 3);
}

static void mc_frame()
{
	for (const std::string &m : g_out)
	{
		if (m.find("\"t\":\"cam\"") != std::string::npos)
		{
			g_mc.walk = m.find("\"walk\":true") != std::string::npos;
			g_mc.input = int(num_after(m, "in"));
			const size_t r = m.find("\"r\":[");
			g_mc.yaw = float(std::atof(m.c_str() + r + 5));
			if (!g_mc.placed)
			{
				const size_t pl = m.find("\"pl\":[");
				sscanf(m.c_str() + pl + 6, "%lf,%lf,%lf", &g_mc.x, &g_mc.y, &g_mc.z);
			}
		}
		else if (m.find("\"t\":\"pset\"") != std::string::npos)
		{
			g_mc.ps = int(num_after(m, "id"));
			const size_t p = m.find("\"pos\":[");
			double x, y, z;
			sscanf(m.c_str() + p + 7, "%lf,%lf,%lf", &x, &y, &z);
			g_mc.x = x;
			g_mc.z = z;
			if (m.find("\"keepY\":false") != std::string::npos)
				g_mc.y = y;
			g_mc.placed = true;
		}
	}
	if (!g_mc.walk)
	{
		g_mc.placed = false;
		return;
	}
	const double yr = g_mc.yaw * 3.14159265 / 180.0, dt = 0.016;
	double vx = 0, vz = 0;
	if (g_mc.input & 1)
	{
		vx = -std::sin(yr) * 4.3;
		vz = std::cos(yr) * 4.3;
	}
	g_mc.x += vx * dt;
	g_mc.z += vz * dt;
	const double vy = g_mc.falling ? -10.0 : 0.0;
	g_mc.y += vy * dt;
	char buf[256];
	snprintf(buf, sizeof(buf), "{\"t\":\"mcpos\",\"pos\":[%.4f,%.4f,%.4f],\"vel\":[%.3f,%.3f,%.3f],\"tn\":%lld,\"w\":1,\"ps\":%d,\"g\":%d}", g_mc.x,
		g_mc.y, g_mc.z, vx, vy, vz, (long long)g_nanos, g_mc.ps, g_mc.falling ? 0 : 1);
	g_in.push_back(buf);
}

static int fails = 0;
static void check(bool ok, const char *what)
{
	std::printf("%s %s\n", ok ? "OK:  " : "FAIL:", what);
	fails += !ok;
}

static void frame()
{
	g_out.clear();
	tick();
	mc_frame();
	g_justPressed.clear();
	g_nanos += 16000000LL;
	g_ms += 16;
	++g_frame;
}

/// The player turns to face GTA heading `h` (the free-look camera while it's on, GTA's own camera otherwise).
static void look(float h)
{
	g_camHeading = h;
	if (g_walkCam.cam != 0)
		g_walkCam.heading = h;
}

static int count_out(const char *needle)
{
	int n = 0;
	for (const std::string &m : g_out)
		n += m.find(needle) != std::string::npos;
	return n;
}

int main()
{
	// the world: a floor with its top at 10.4, a tall wall east (x 5..6), a 2 m ledge west (x -6..-5), a 1 m wall
	// north (y 5..6)
	g_world = {{-50, -50, 9.4f, 50, 50, 10.4f, 0}, {5, -10, 10.4f, 6, 10, 14.4f, 0}, {-6, -10, 10.4f, -5, 10, 12.4f, 0},
		{-3, 5, 10.4f, 3, 6, 11.4f, 0}};
	g_ws.start("127.0.0.1", 0);
	g_sped = {};

	// frames until Minecraft's movement is on and Minecraft has taken over
	int psets = 0, hcs = 0;
	bool hcFloor = false;
	for (int i = 0; i < 10; ++i)
	{
		frame();
		psets += 0;
	}
	check(g_walk.on, "Minecraft's movement turns on (on foot, in control)");
	check(g_sped.frozen, "GTA's player is frozen while Minecraft moves it");
	check(g_walk.acked, "Minecraft's positions count once it has the host's correction");
	check(std::fabs(g_yOffset - (-0.4f)) < 1e-4f, "ground levelled: yOffset -0.4");

	// the floor boxes sent to Minecraft: top at 10.4 + yOffset = 10.0
	g_out.clear();
	g_proxy.dirty = true;
	g_proxy.nextSendAt = 0;
	proxy_tick(1, true);
	for (const std::string &m : g_out)
		if (m.find("\"t\":\"hc\"") != std::string::npos)
		{
			++hcs;
			hcFloor = m.find(",10.00,") != std::string::npos;
		}
	check(hcs == 1 && hcFloor, "collision boxes sent, the floor's top at Minecraft y 10.00");

	// walking east (GTA heading -90, Minecraft yaw 270) into the wall at x 5: Minecraft walks through, GTA stops it
	look(-90.0f);
	g_pressed = {32};
	float maxX = -100;
	int corrections = 0;
	bool wallBox = false;
	for (int i = 0; i < 120; ++i)
	{
		frame();
		maxX = std::max(maxX, g_sped.x);
		corrections += count_out("\"keepY\":true");
		for (const std::string &m : g_out)
			if (m.find("\"t\":\"hc\"") != std::string::npos && m.find("[5.00,") != std::string::npos)
				wallBox = true;
			else if (m.find("\"t\":\"hc\"") != std::string::npos && m.find(",5.00,") != std::string::npos)
				wallBox = true;
	}
	g_pressed.clear();
	std::printf("      ped x %.3f (max %.3f), Minecraft x %.3f, corrections %d\n", g_sped.x, maxX, g_mc.x, corrections);
	check(g_walk.x > 4.0f, "walked east with Minecraft");
	check(maxX < 4.75f, "GTA's player never goes into the wall (stopped 0.3 m short)");
	check(corrections > 0, "Minecraft is told where it was stopped (pset keepY)");
	check(wallBox, "the wall is among the collision boxes (x 5)");
	check(std::fabs(g_mc.x - g_sped.x) < 0.3, "Minecraft's player is back at the wall too");

	// a Minecraft teleport (chorus fruit, under 20 m): GTA's player follows
	g_mc.x = -4.3;
	for (int i = 0; i < 5; ++i)
		frame();
	check(std::fabs(g_sped.x - (-4.3f)) < 0.1f, "GTA's player follows a short Minecraft teleport");

	// Space tapped facing the 2 m ledge 0.7 m away: only Minecraft's jump
	look(90.0f); // west
	g_calls.clear();
	g_justPressed = {22};
	g_pressed = {22};
	frame();
	g_pressed.clear();
	frame();
	bool climbed = std::find(g_calls.begin(), g_calls.end(), "TaskClimb") != g_calls.end();
	check(!climbed && g_walk.on, "Space tapped at a 2 m ledge: just Minecraft's jump (no GTA climb)");
	// Space held there: GTA climbs it
	g_justPressed = {22};
	g_pressed = {22};
	for (int i = 0; i < 30 && !climbed; ++i)
	{
		frame();
		climbed = std::find(g_calls.begin(), g_calls.end(), "TaskClimb") != g_calls.end();
	}
	g_pressed.clear();
	check(climbed, "Space held at a 2 m ledge: GTA climbs it (TaskClimb)");
	check(!g_walk.on && !g_sped.frozen, "GTA has the player (unfrozen) while it climbs");
	int back = -1;
	for (int i = 0; i < 120 && back < 0; ++i)
	{
		frame();
		if (g_walk.on)
			back = i;
	}
	check(back > 60, "Minecraft's movement comes back after the climb (about a second)");

	// Space facing the 1 m wall: Minecraft jumps (no GTA climb), the jump goes to Minecraft
	g_mc.x = 0.0;
	g_mc.z = -4.4; // GTA y 4.4
	for (int i = 0; i < 5; ++i)
		frame();
	look(0.0f); // north
	g_calls.clear();
	g_justPressed = {22};
	g_pressed = {22};
	for (int i = 0; i < 30; ++i)
		frame();
	g_pressed.clear();
	climbed = std::find(g_calls.begin(), g_calls.end(), "TaskClimb") != g_calls.end();
	check(!climbed && g_walk.on, "Space held at a 1 m wall: no GTA climb (Minecraft's jump does it)");
	check(count_out("\"in\":16") > 0, "the jump key goes to Minecraft");

	// a mission moves GTA's player: Minecraft's goes there too
	g_sped.x = 20.0f;
	g_sped.y = 20.0f;
	frame();
	check(count_out("\"t\":\"pset\"") > 0 && std::fabs(g_walk.x - 20.0f) < 0.01f, "a mission's teleport of GTA's player moves Minecraft's");
	for (int i = 0; i < 5; ++i)
		frame();
	check(std::fabs(g_sped.x - 20.0f) < 0.1f && std::fabs(g_mc.x - 20.0) < 0.1, "both stay at the mission's spot");

	// E: Minecraft's inventory
	g_justPressed = {51};
	frame();
	check(count_out("\"k\":\"inventory\",\"down\":true") > 0, "E opens Minecraft's inventory");

	// F with no car about: GTA tries a ladder
	g_calls.clear();
	g_justPressed = {23};
	frame();
	check(std::find(g_calls.begin(), g_calls.end(), "TaskClimbLadder") != g_calls.end() && !g_walk.on, "F: GTA climbs a ladder (if there is one)");
	for (int i = 0; i < 80; ++i)
		frame();
	check(g_walk.on, "and Minecraft's movement is back after");

	// a cutscene: GTA has the player, Minecraft's HUD hides, Steve still stands in
	g_cutscene = true;
	frame();
	check(!g_walk.on && !g_sped.frozen, "a cutscene: GTA has the player");
	check(count_out("\"t\":\"hud\",\"hidden\":true") == 1, "Minecraft's HUD hides in the cutscene");
	check(count_out("\"walk\":false") > 0, "the camera message says GTA moves the player");
	g_cutscene = false;
	frame();
	check(count_out("\"t\":\"hud\",\"hidden\":false") == 1, "and shows again after");

	// Minecraft's player falls through GTA's floor: put back up; twice: GTA's movement takes over
	g_mc.falling = true;
	int resets = 0;
	for (int i = 0; i < 40; ++i)
	{
		frame();
		resets += count_out("\"keepY\":false");
	}
	std::printf("      resets %d, Minecraft y %.2f, ped z %.2f, walk %d\n", resets, g_mc.y, g_sped.z, int(g_walk.on));
	check(resets >= 1, "fell through the floor: put back on it");
	g_mc.falling = false;
	for (int i = 0; i < 10; ++i)
		frame();
	g_mc.falling = true;
	for (int i = 0; i < 40; ++i)
		frame();
	g_mc.falling = false;
	for (int i = 0; i < 30; ++i)
		frame();
	std::printf("      after: Minecraft y %.2f, ped z %.2f, walk %d, ground %d\n", g_mc.y, g_sped.z, int(g_walk.on), int(g_walk.ground));
	check(g_mcMove && g_walk.on && g_sped.z > 10.9f, "falling through again soon: put back on the floor again, Minecraft's movement stays on");

	// (Minecraft's movement off and on again, F6: a fresh start for what follows)
	g_mcMove = false;
	for (int i = 0; i < 5; ++i)
		frame();
	g_mcMove = true;
	for (int i = 0; i < 5; ++i)
		frame();
	check(g_walk.on, "F6 back on: Minecraft's movement again");

	// a car about to run into the player: GTA has the player
	g_car = {true, g_sped.x + 6.0f, g_sped.y, 11.0f, -15.0f, 0.0f};
	frame();
	check(!g_walk.on && !g_sped.frozen, "a car coming: GTA's physics has the player");
	g_car.on = false;
	for (int i = 0; i < 120; ++i)
		frame();
	check(g_walk.on, "and Minecraft's movement is back after");

	// a door in the way: pushed open while walking into it
	g_world.push_back({g_sped.x + 0.5f, g_sped.y - 0.5f, 10.4f, g_sped.x + 0.6f, g_sped.y + 0.5f, 12.4f, 50});
	g_types[50] = 3;
	look(-90.0f);
	g_calls.clear();
	g_pressed = {32};
	frame();
	g_pressed.clear();
	check(std::find(g_calls.begin(), g_calls.end(), "ApplyForce:50") != g_calls.end(), "walking into a door pushes it");

	// Shift sprints, Ctrl sneaks (and flies down)
	g_pressed = {21};
	frame();
	g_pressed.clear();
	check(count_out("\"in\":64") > 0, "Shift: Minecraft sprints");
	g_pressed = {36};
	frame();
	g_pressed.clear();
	check(count_out("\"in\":32") > 0, "Ctrl: Minecraft sneaks (flying: down)");

	// free look: the mouse looks all the way down (GTA's own camera stops well short)
	check(g_walkCam.cam != 0, "Minecraft's free look renders while Minecraft moves the player");
	g_kbm = true;
	g_look[2] = 40.0f;
	frame();
	g_look[2] = 0.0f;
	check(g_walkCam.pitch < -89.0f && count_out(",89.500,") > 0, "the mouse looks straight down (pitch -89.5, Minecraft 89.5)");
	g_look[2] = -60.0f;
	frame();
	g_look[2] = 0.0f;
	check(g_walkCam.pitch > 89.0f, "and straight up");
	g_look[2] = 15.0f;
	frame();
	g_look[2] = 0.0f;
	g_kbm = false;

	// the minimap's mask: off by default (Minecraft's blocks show there too)
	frame();
	check(g_mask[2] <= g_mask[0], "no minimap mask by default");
	// KeepMinimap=1: on while GTA draws it, off in a cutscene (Minecraft shows there then)
	g_settings.keepMinimap = true;
	frame();
	check(g_mask[2] > g_mask[0], "the minimap is masked while it shows");
	g_cutscene = true;
	frame();
	check(!(g_mask[2] > g_mask[0]), "no mask in a cutscene");
	check(count_out("\"k\":\"attack\",\"down\":false") == 1 && count_out("\"k\":\"use\",\"down\":false") == 1,
		"a cutscene lets go of Minecraft's attack and use");
	check(count_out("\"ctl\":false") > 0, "and tells Minecraft it gets no input");
	frame();
	check(count_out("\"k\":\"attack\"") == 0, "(once)");
	g_justPressed = {24};
	frame();
	check(count_out("\"k\":\"attack\",\"down\":true") == 0, "no clicks reach Minecraft in a cutscene");
	g_cutscene = false;
	frame();
	check(count_out("\"ctl\":true") > 0, "input again after it");

	// F6: GTA's own movement
	g_toggleJump = true;
	frame();
	check(!g_walk.on && !g_sped.frozen, "F6: GTA's own movement");
	check(g_walkCam.cam == 0, "and GTA's own camera");

	// GTA moves the player: Steve's body follows its bones
	frame();
	std::string cam;
	for (const std::string &m : g_out)
		if (m.find("\"t\":\"cam\"") != std::string::npos)
			cam = m;
	const size_t rigAt = cam.find("\"rig\":[");
	check(rigAt != std::string::npos, "the camera message carries Steve's pose (rig)");
	if (rigAt != std::string::npos)
	{
		float v[24] = {};
		const char *q = cam.c_str() + rigAt + 7;
		for (int i = 0; i < 24; ++i)
		{
			v[i] = std::strtof(q, const_cast<char **>(&q));
			if (*q == ',')
				++q;
		}
		std::printf("      up %.2f %.2f %.2f  right %.2f %.2f %.2f  head fwd %.2f %.2f %.2f  left arm %.2f %.2f %.2f\n", v[0], v[1], v[2], v[3],
			v[4], v[5], v[6], v[7], v[8], v[12], v[13], v[14]);
		check(v[1] > 0.99f && std::fabs(v[3] - 1.0f) < 0.01f, "body up is Minecraft's up, right is east (+x)");
		check(v[8] < -0.98f && std::fabs(v[7]) < 0.05f, "the head looks north (Minecraft -z), level");
		check(v[13] < -0.95f, "arms hang down");
		const size_t pl = cam.find("\"pl\":[");
		double fx, fy, fz;
		sscanf(cam.c_str() + pl + 6, "%lf,%lf,%lf", &fx, &fy, &fz);
		std::printf("      feet %.3f %.3f %.3f (ped z %.3f, yOffset %.3f)\n", fx, fy, fz, g_sped.z, g_yOffset);
		check(std::fabs(fy - (g_sped.z - 1.0f + g_yOffset)) < 0.03, "standing: Steve's feet on the ground");
	}

	// a character switch: the one left behind shows again, unfrozen
	g_toggleJump = true;
	for (int i = 0; i < 5; ++i)
		frame();
	check(g_walk.on && g_frozenOf[1], "(Minecraft's movement again, the player frozen)");
	g_playerPed = 2;
	frame();
	check(g_visible[1] && !g_frozenOf[1], "a character switch: the old character shows again and isn't frozen");
	g_playerPed = 1;
	for (int i = 0; i < 5; ++i)
		frame();

	// (the door is gone)
	g_world.erase(std::remove_if(g_world.begin(), g_world.end(), [](const Box &b) { return b.entity == 50; }), g_world.end());
	// an arrow at one of Minecraft's own blocks (its GTA prop): Minecraft stops it, GTA reports no hit
	g_world.push_back({g_sped.x - 0.5f, g_sped.y + 3.0f, 10.4f, g_sped.x + 0.5f, g_sped.y + 4.0f, 11.4f, 60});
	g_types[60] = 3;
	g_props.handles.insert(60);
	{
		char m[200];
		snprintf(m, sizeof(m), "{\"t\":\"proj\",\"p\":[[901,\"arrow\",%.3f,%.3f,%.3f]]}", g_sped.x, 10.9 + g_yOffset, -(g_sped.y + 6.0));
		g_in.push_back(m);
	}
	frame();
	check(count_out("\"t\":\"projhit\"") == 0, "an arrow at Minecraft's own block: GTA leaves it to Minecraft");
	g_props.handles.erase(60);
	g_world.pop_back();

	// an arrow into a car's front wheel: the tyre bursts
	g_car = {true, g_sped.x + 4.0f, g_sped.y, 11.0f, 0.0f, 0.0f};
	g_world.push_back({g_car.x - 1.0f, g_car.y - 2.3f, 10.4f, g_car.x + 1.0f, g_car.y + 2.3f, 11.9f, 70});
	g_types[70] = 2;
	g_calls.clear();
	g_in.push_back("{\"t\":\"proj\",\"p\":[]}");
	frame();
	{
		// from the player's chest (3 m west of the car's side) through its front left wheel, 1.3 m ahead of its middle
		char m[200];
		snprintf(m, sizeof(m), "{\"t\":\"proj\",\"p\":[[902,\"arrow\",%.3f,%.3f,%.3f]]}", g_car.x + 2.0f, 9.38 + g_yOffset, -(g_car.y + 2.6));
		g_in.push_back(m);
	}
	frame();
	check(std::find(g_calls.begin(), g_calls.end(), "TyreBurst:0") != g_calls.end(), "an arrow in a car's wheel bursts that tyre");
	g_world.pop_back();
	g_car.on = false;

	// ---- a lead ----
	// out in the open (GTA y -20), someone slim 3 m north of the player
	g_sped.x = 0.0f;
	g_sped.y = -20.0f;
	for (int i = 0; i < 6; ++i)
		frame();
	check(std::fabs(g_walk.y - (-20.0f)) < 0.05f && std::fabs(g_mc.z - 20.0) < 0.05 && g_walk.acked,
		"a mission's teleport of over 20 m: Minecraft's player goes there and moves on from there");
	look(0.0f);
	g_camPitch = g_walkCam.pitch = 0.0f;
	g_npcs[5] = {g_walk.x, g_walk.y + 3.0f, 11.4f};
	g_npcs[5].model = Hash(std::hash<std::string>{}("a_m_y_hipster_01"));
	g_types[5] = 1;
	auto npcBox = [] {
		g_world.erase(std::remove_if(g_world.begin(), g_world.end(), [](const Box &b) { return b.entity == 5; }), g_world.end());
		const SimNpc &q = g_npcs[5];
		g_world.push_back({q.x - 0.3f, q.y - 0.3f, q.z - 1.0f, q.x + 0.3f, q.y + 0.3f, q.z + 0.8f, 5});
	};
	npcBox();
	g_in.push_back("{\"t\":\"leashuse\",\"item\":\"lead\",\"mobs\":0}");
	frame();
	frame();
	check(g_leashes.size() == 1 && g_leashes[0].a.e == 5 && g_leashes[0].b.kind == 0, "a lead put on someone: in Steve's hand");
	check(count_out("\"t\":\"leashes\",\"l\":[[") > 0 && count_out(",0,") > 0, "Minecraft is told where the lead's ends are (held: kind 0)");
	// walking off south: a slim person is led along
	const float startY = g_npcs[5].y;
	look(180.0f);
	g_calls.clear();
	g_pressed = {32};
	for (int i = 0; i < 70; ++i)
	{
		frame();
		npcBox();
	}
	g_pressed.clear();
	const float led = startY - g_npcs[5].y;
	std::printf("      walked %.2f m, the person came %.2f m, lead %.2f m\n", -20.0f - g_walk.y, led, g_leashes.empty() ? 0.0f : g_leashes[0].length);
	check(led > 2.0f, "a slim person on the lead is pulled along");
	check(std::find(g_calls.begin(), g_calls.end(), "Walk:5") != g_calls.end(), "and walks after Steve");
	// a heavy-set one: Steve can't pull them, he's held back
	g_npcs[5].model = Hash(std::hash<std::string>{}("a_m_m_fatlatin_01"));
	const float heavyY = g_npcs[5].y;
	int held = 0;
	g_pressed = {32};
	for (int i = 0; i < 70; ++i)
	{
		frame();
		npcBox();
		held += count_out("\"keepY\":true");
	}
	g_pressed.clear();
	for (int i = 0; i < 3; ++i)
		frame();
	const float gap = std::fabs(g_walk.y - g_npcs[5].y);
	std::printf("      heavy: moved %.2f m, Steve %.2f m from them, held back %d times\n", heavyY - g_npcs[5].y, gap, held);
	check(std::fabs(heavyY - g_npcs[5].y) < 0.05f, "a heavy-set person doesn't budge");
	check(held > 0 && gap < g_leashes[0].length + 1.6f, "and Steve is held back by them");
	// tied to the street: aimed at the floor, with the lead in hand
	look(0.0f);
	g_camPitch = g_walkCam.pitch = -35.0f;
	g_in.push_back("{\"t\":\"leashuse\",\"item\":\"lead\",\"mobs\":0}");
	frame();
	frame();
	check(!g_leashes.empty() && g_leashes[0].b.kind == 2, "the lead tied to the street (a fixed spot)");
	check(count_out("\"e\":\"tied\"") > 0 || count_out(",1,") > 0, "Minecraft is told (tied, kind 1)");
	// shears at the person: cut
	g_camPitch = g_walkCam.pitch = 0.0f;
	g_in.push_back("{\"t\":\"leashuse\",\"item\":\"shears\",\"mobs\":0}");
	frame();
	check(g_leashes.empty() && count_out("\"e\":\"cut\"") == 1, "shears cut the lead");
	frame();
	check(count_out("\"t\":\"leashes\",\"l\":[]") == 1, "and Minecraft is told there are none left");

	// ---- a boat ----
	// one of Minecraft's boats where the person stands: they get in (Minecraft gives them a seat), and sit there
	{
		const SimNpc &q = g_npcs[5];
		char m[300];
		snprintf(m, sizeof(m), "{\"t\":\"boats\",\"b\":[[9,%.3f,%.3f,%.3f,0,0,0,0,2,1]],\"s\":[]}", q.x, q.z - 1.0f + g_yOffset, -q.y);
		g_in.push_back(m);
		frame();
		frame();
		check(count_out("\"t\":\"boatgrab\",\"boat\":9,\"ped\":5") == 1 || g_riders.count(5), "someone walking into a boat with room gets in");
		snprintf(m, sizeof(m), "{\"t\":\"boats\",\"b\":[[9,%.3f,%.3f,%.3f,0,0,0,0,1,1]],\"s\":[[5,9,%.3f,%.3f,%.3f,0]]}", q.x + 2.0f, q.z - 1.0f + g_yOffset,
			-q.y, q.x + 2.0f, q.z - 0.8f + g_yOffset, -q.y);
		g_calls.clear();
		g_in.push_back(m);
		frame();
		frame();
		check(g_riders.count(5) && g_riders[5].seated && g_npcs[5].frozen, "Minecraft gave them the seat: sat in it");
		check(std::find(g_calls.begin(), g_calls.end(), "Sit:5") != g_calls.end(), "sitting down");
		std::printf("      seat x %.2f, person x %.2f\n", g_riders.count(5) ? g_riders[5].x : 0.0f, g_npcs[5].x);
		check(g_riders.count(5) && std::fabs(g_npcs[5].x - g_riders[5].x) < 0.05f, "carried where the boat is");
		// the boat breaks (Minecraft lists no seat): out they come
		g_in.push_back("{\"t\":\"boats\",\"b\":[],\"s\":[]}");
		for (int i = 0; i < 30; ++i)
			frame();
		check(!g_riders.count(5) && !g_npcs[5].frozen, "the boat broken: out again");
	}
	g_npcs.clear();
	g_world.erase(std::remove_if(g_world.begin(), g_world.end(), [](const Box &b) { return b.entity == 5; }), g_world.end());

	// ---- a sign punched: flung, with hundreds of other things about (it isn't among the first listed) ----
	look(0.0f);
	g_camPitch = g_walkCam.pitch = 0.0f;
	for (int i = 0; i < 400; ++i)
		g_objects.push_back(1000 + i);
	g_objects.push_back(301);
	g_types[301] = 3;
	g_objPos[301] = {g_walk.x, g_walk.y + 2.0f, g_walk.z + 1.0f};
	g_world.push_back({g_walk.x - 0.3f, g_walk.y + 1.8f, g_walk.z, g_walk.x + 0.3f, g_walk.y + 2.2f, g_walk.z + 2.5f, 301});
	g_calls.clear();
	g_in.push_back("{\"t\":\"melee\",\"k\":\"fist\",\"s\":1.00,\"kb\":0,\"sh\":0,\"fa\":0,\"lo\":0}");
	frame();
	check(std::find(g_calls.begin(), g_calls.end(), "ApplyForce:301") != g_calls.end(), "a sign the crosshair is on is knocked flying (among 400 other things)");
	check(std::count_if(g_calls.begin(), g_calls.end(), [](const std::string &c) { return c.rfind("ApplyForce:", 0) == 0; }) == 1,
		"and nothing else (a swing hits what it's on)");
	check(std::find(g_calls.begin(), g_calls.end(), "Velocity:301") == g_calls.end(), "pushed, never its velocity set (that crashed GTA on map objects)");
	g_world.pop_back();
	g_objects.clear();

	// ---- a pickup in reach: GTA's player is put on it to collect it, Minecraft's player isn't moved ----
	{
		g_objects = {401};
		g_types[401] = 3;
		g_pickups = {401};
		g_objPos[401] = {g_walk.x + 1.4f, g_walk.y, g_walk.z + 0.1f};
		int psets = 0;
		float furthest = 0.0f;
		for (int i = 0; i < 40; ++i)
		{
			frame();
			psets += count_out("\"t\":\"pset\"");
			furthest = std::max(furthest, std::fabs(g_sped.x - g_walk.x));
		}
		check(furthest > 1.0f, "GTA's player is put on a pickup in reach (GTA collects it)");
		check(psets == 0, "and Minecraft's player stays where it is (no correction)");
		g_objects.clear();
		g_pickups.clear();
	}

	// ---- walking onto ground that's off a whole block: no relevel under way ----
	{
		g_world.push_back({g_walk.x - 3.0f, g_walk.y + 1.0f, 9.4f, g_walk.x + 3.0f, g_walk.y + 30.0f, 10.73f, 0});
		look(0.0f);
		g_pressed = {32};
		int clears = 0;
		for (int i = 0; i < 300; ++i)
		{
			frame();
			clears += count_out("\"t\":\"clear\"");
		}
		g_pressed.clear();
		check(clears == 0, "walking over uneven ground: Minecraft's ground isn't rebuilt under way");
		g_world.pop_back();
	}

	// ---- /kill @e: someone sitting in a car ----
	g_npcs[6] = {g_walk.x + 5.0f, g_walk.y, 11.4f};
	g_npcs[6].inCar = true;
	g_types[6] = 1;
	g_calls.clear();
	g_in.push_back("{\"t\":\"gtacmd\",\"c\":\"kill\",\"what\":\"all\"}");
	frame();
	for (int i = 0; i < 40; ++i)
		frame();
	check(std::find(g_calls.begin(), g_calls.end(), "Out:6") != g_calls.end(), "/kill @e: someone in a car still alive shortly after is taken out of it and killed");
	g_npcs.clear();

	// ---- mining GTA's world ----
	{
		for (int i = 0; i < 4; ++i)
			frame();
		// (solid ground under the street here, as GTA's: no underside to see from below)
		const Box street = g_world[0];
		g_world[0].z0 = -30.0f;
		// the street cell under the player's feet (Minecraft y 9: GTA 9.4..10.4, the floor box's top layer)
		const int cx = int(std::floor(g_walk.x)) + 2, cz = int(std::floor(-g_walk.y)), cy = 9;
		char m[160];
		snprintf(m, sizeof(m), "{\"t\":\"dug\",\"c\":[%d,%d,%d],\"k\":1}", cx, cy, cz);
		g_in.push_back(m);
		frame();
		check(g_dug.count(cell_key(cx, cy, cz)) == 1, "a cell of GTA's street mined: dug out");
		int fills = 0;
		std::string fillMsg;
		for (const std::string &o : g_out)
			if (o.find("\"t\":\"digfill\"") != std::string::npos)
				fillMsg = o;
		for (size_t at = fillMsg.find('['); at != std::string::npos && at < fillMsg.size(); at = fillMsg.find(',', at + 1))
			++fills;
		std::printf("      %s\n", fillMsg.c_str());
		char below[48], above[48];
		snprintf(below, sizeof(below), "%d,%d,%d,", cx, cy - 1, cz);
		snprintf(above, sizeof(above), "%d,%d,%d,", cx, cy + 1, cz);
		check(fillMsg.find(below) != std::string::npos && fillMsg.find(above) == std::string::npos && g_digFill.size() == 5,
			"the ground round it (below and beside) filled with Minecraft's blocks; the air above not");
		frame(); // (sent with the next frame)
		check(compositor::g_digCells == 6, "the shader gets the dug cell and the ones round it");
		Vector3 at = {}, n = {};
		const bool overHole = solid_probe(1, cx + 0.5f, -(cz + 0.5f), 12.0f, cx + 0.5f, -(cz + 0.5f), 8.0f, at, n);
		const bool beside = solid_probe(1, cx + 3.5f, -(cz + 0.5f), 12.0f, cx + 3.5f, -(cz + 0.5f), 8.0f, at, n);
		check(!overHole && beside, "GTA's street isn't there over the hole (the player can go down it), still beside it");
		// a door (one of GTA's things) where the crosshair is: hidden, not dug
		g_types[501] = 3;
		g_world.push_back({cx + 5.0f, -(cz + 1.0f), 10.4f, cx + 5.1f, -float(cz), 12.4f, 501});
		snprintf(m, sizeof(m), "{\"t\":\"dug\",\"c\":[%d,%d,%d],\"k\":3}", cx + 5, 10, cz);
		g_in.push_back(m);
		frame();
		check(g_visible.count(501) && !g_visible[501] && !g_dug.count(cell_key(cx + 5, 10, cz)), "a door mined: it's taken away (hidden), not dug");
		g_world.pop_back();
		// /clearall: all back
		g_in.push_back("{\"t\":\"gtacmd\",\"c\":\"clearall\"}");
		frame();
		frame();
		check(g_dug.empty() && g_digFill.empty() && g_visible[501] && compositor::g_digCells == 0, "/clearall: nothing dug any more, the door back");
		// under a bridge (the street as a slab with open air under it): the cell under it isn't filled
		g_world[0] = street;
		snprintf(m, sizeof(m), "{\"t\":\"dug\",\"c\":[%d,%d,%d],\"k\":0}", cx, cy, cz);
		g_in.push_back(m);
		frame();
		check(g_dug.size() == 1 && !g_digFill.count(cell_key(cx, cy - 1, cz)), "under a bridge's deck: the open air below isn't filled");
		g_in.push_back("{\"t\":\"gtacmd\",\"c\":\"clearall\"}");
		frame();
	}

	// ---- Minecraft's fire: GTA's people by it catch fire, and it's GTA's fire too ----
	{
		g_npcs[7] = {g_walk.x + 3.0f, g_walk.y, 11.4f};
		g_types[7] = 1;
		const int fx = int(std::floor(g_walk.x + 3.6f)), fz = int(std::floor(-g_walk.y)), fy = 10;
		char m[96];
		snprintf(m, sizeof(m), "{\"t\":\"hot\",\"fire\":[%d,%d,%d]}", fx, fy, fz);
		g_calls.clear();
		g_in.push_back(m);
		for (int i = 0; i < 20; ++i)
			frame();
		check(std::find(g_calls.begin(), g_calls.end(), "Fire:7") != g_calls.end(), "someone standing by Minecraft's fire catches fire");
		check(std::find(g_calls.begin(), g_calls.end(), "ScriptFire") != g_calls.end(), "and it burns as GTA's own fire too");
		snprintf(m, sizeof(m), "{\"t\":\"hot\",\"clear\":[%d,%d,%d]}", fx, fy, fz);
		g_in.push_back(m);
		for (int i = 0; i < 20; ++i)
			frame();
		check(std::find(g_calls.begin(), g_calls.end(), "ScriptFireOut") != g_calls.end(), "out with Minecraft's fire");
		g_npcs.clear();
	}
	std::printf("%s\n", fails == 0 ? "ALL PASSED" : "SOME FAILED");
	return fails != 0;
}
