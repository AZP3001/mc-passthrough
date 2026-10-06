#include "compositor.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>
#include <reshade.hpp>

using namespace reshade::api;

namespace
{
	constexpr const wchar_t *kMappingName = L"Local\\MCPassthroughFrame";
	constexpr uint32_t kMagic = 0x5450434D; // "MCPT"
	constexpr int kHeader = 4096;
	constexpr int kSlotDesc = 256;
	constexpr int kSlotDescBytes = 128;
	constexpr const char *kEffect = "MCPassthrough.fx";

	std::atomic<bool> g_registered{false};
	std::atomic<bool> g_active{false};
	std::atomic<float> g_hostNear{0.15f};
	std::atomic<float> g_hostFar{10000.0f};
	std::atomic<uint32_t> g_bbWidth{0}, g_bbHeight{0};
	std::atomic<bool> g_cameraLocked{false};
	std::atomic<float> g_lookLight{-1.0f}, g_lookBias{-1.0f}, g_lookSlope{-1.0f};
	std::atomic<float> g_shakeX{0.0f}, g_shakeY{0.0f}, g_shakeRoll{0.0f}, g_portalWarp{0.0f};
	std::atomic<ULONGLONG> g_activeAt{0}; // when the script last said whether to composite
	std::atomic<float> g_maskX0{0.0f}, g_maskY0{0.0f}, g_maskX1{0.0f}, g_maskY1{0.0f};
	std::atomic<float> g_cursorX{0.0f}, g_cursorY{0.0f}, g_cursorOn{0.0f};
	float g_savedLight = -1.0f, g_savedBias = -1.0f, g_savedSlope = -1.0f;

	struct Pose
	{
		float yaw = 0, pitch = 0, roll = 0, fov = 70;
		double x = 0, y = 0, z = 0;
		bool valid = false;
	};
	std::mutex g_poseLock;
	// GTA's recent camera poses, newest last. The script reads the camera GTA is about to render (the next frame),
	// while the picture being presented was rendered with the one before: re-project to that (g_poseLag back).
	Pose g_hostPoses[4];
	unsigned g_hostPoseCount = 0;
	std::atomic<int> g_poseLag{0}; // measured: 0 matches best (third and first person)
	Pose g_mcPose;
	double g_mcSteve[3] = {0, 0, 0}; // Steve's feet when Minecraft rendered its frame
	bool g_mcSteveValid = false;
	struct Steve
	{
		float box[4] = {0, 0, 0, 0}, depth[3] = {0, 0, 0};
		float carFar = 0.0f;
		double pos[3] = {0, 0, 0};
		float height = 1.8f;
		bool seated = false;
		bool valid = false;
	} g_steve; // under g_poseLock

	HANDLE g_mapping = nullptr;
	const uint8_t *g_view = nullptr;
	int32_t g_slots = 0;
	int64_t g_stride = 0;
	int64_t g_lastPublish = -1;
	ULONGLONG g_nextOpenAttempt = 0;

	struct Layer
	{
		resource tex = {0};
		resource_view srv = {0};
	};
	Layer g_world, g_depth, g_overlay;
	uint32_t g_width = 0, g_height = 0;
	// the cells mined out of GTA's world (set_dig), and the texture the effect reads them from
	std::mutex g_digLock;
	std::vector<uint8_t> g_digCells; // under g_digLock; empty: none
	int g_digOrigin[3] = {0, 0, 0};  // under g_digLock
	bool g_digChanged = false;       // under g_digLock
	Layer g_dig;
	bool g_digOn = false;
	int g_digShown[3] = {0, 0, 0};
	bool g_hasFrame = false;
	float g_mcNear = 0.05f, g_mcFar = 2048.0f;
	int32_t g_mcFlags = 7;

	template <typename T>
	T read(const uint8_t *p)
	{
		T v;
		std::memcpy(&v, p, sizeof(T));
		return v;
	}

	bool open_mapping()
	{
		if (g_view != nullptr)
			return true;
		if (GetTickCount64() < g_nextOpenAttempt)
			return false;
		g_nextOpenAttempt = GetTickCount64() + 1000;
		g_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kMappingName);
		if (g_mapping == nullptr)
			return false;
		const auto *header = static_cast<const uint8_t *>(MapViewOfFile(g_mapping, FILE_MAP_READ, 0, 0, kHeader));
		// (only the layout this reads: version 1, and sizes that make sense)
		if (header == nullptr || read<uint32_t>(header) != kMagic || read<uint32_t>(header + 4) != 1 || read<int32_t>(header + 12) < 1 ||
			read<int32_t>(header + 12) > 8 || read<int64_t>(header + 16) <= 0 || read<int64_t>(header + 16) > (int64_t(1) << 30))
		{
			if (header != nullptr)
				UnmapViewOfFile(header);
			CloseHandle(g_mapping);
			g_mapping = nullptr;
			return false;
		}
		g_slots = read<int32_t>(header + 12);
		g_stride = read<int64_t>(header + 16);
		UnmapViewOfFile(header);
		g_view = static_cast<const uint8_t *>(MapViewOfFile(g_mapping, FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(kHeader + g_stride * g_slots)));
		if (g_view == nullptr)
		{
			CloseHandle(g_mapping);
			g_mapping = nullptr;
			return false;
		}
		reshade::log::message(reshade::log::level::info, "MCPassthrough: connected to Minecraft's frame export");
		return true;
	}

	void destroy_layers(device *dev)
	{
		for (Layer *layer : {&g_world, &g_depth, &g_overlay})
		{
			if (layer->srv.handle != 0)
				dev->destroy_resource_view(layer->srv);
			if (layer->tex.handle != 0)
				dev->destroy_resource(layer->tex);
			*layer = Layer();
		}
		g_width = g_height = 0;
		g_hasFrame = false;
	}

	bool create_layer(device *dev, Layer &layer, uint32_t w, uint32_t h, format fmt)
	{
		if (!dev->create_resource(
				resource_desc(w, h, 1, 1, fmt, 1, memory_heap::default_, resource_usage::shader_resource | resource_usage::copy_dest),
				nullptr, resource_usage::shader_resource, &layer.tex))
			return false;
		return dev->create_resource_view(layer.tex, resource_usage::shader_resource, resource_view_desc(fmt), &layer.srv);
	}

	void bind(effect_runtime *runtime)
	{
		runtime->update_texture_bindings("MCWORLD", g_world.srv, g_world.srv);
		runtime->update_texture_bindings("MCDEPTH", g_depth.srv, g_depth.srv);
		runtime->update_texture_bindings("MCOVERLAY", g_overlay.srv, g_overlay.srv);
		if (g_dig.srv.handle != 0)
			runtime->update_texture_bindings("MCDIG", g_dig.srv, g_dig.srv);
	}

	/// The dug cells to the effect's texture, when they changed.
	void upload_dig(effect_runtime *runtime)
	{
		std::lock_guard<std::mutex> lock(g_digLock);
		if (!g_digChanged)
			return;
		g_digChanged = false;
		g_digOn = !g_digCells.empty();
		if (!g_digOn)
			return;
		device *dev = runtime->get_device();
		if (g_dig.tex.handle == 0)
		{
			if (!create_layer(dev, g_dig, 512, 512, format::r8_unorm))
			{
				g_dig = Layer();
				g_digOn = false;
				return;
			}
			runtime->update_texture_bindings("MCDIG", g_dig.srv, g_dig.srv);
		}
		subresource_data data;
		data.data = g_digCells.data();
		data.row_pitch = 512;
		data.slice_pitch = 512 * 512;
		dev->update_texture_region(data, g_dig.tex, 0);
		std::memcpy(g_digShown, g_digOrigin, sizeof(g_digShown));
	}

	/// Upload the newest published Minecraft frame, if there is one we haven't shown yet.
	void upload(effect_runtime *runtime)
	{
		const int64_t published = read<int64_t>(g_view + 32);
		if (published == g_lastPublish)
			return;
		const int32_t slot = read<int32_t>(g_view + 40);
		if (slot < 0 || slot >= g_slots)
			return;
		const uint8_t *desc = g_view + kSlotDesc + kSlotDescBytes * slot;
		const int64_t seq = read<int64_t>(desc);
		if (seq & 1)
			return;
		const uint32_t w = read<uint32_t>(desc + 24), h = read<uint32_t>(desc + 28);
		if (w == 0 || h == 0 || w > 16384 || h > 16384 || int64_t(w) * h * 4 * 3 > g_stride)
			return; // (three layers of it must fit in the slot)
		device *dev = runtime->get_device();
		if (w != g_width || h != g_height)
		{
			destroy_layers(dev);
			if (!create_layer(dev, g_world, w, h, format::r8g8b8a8_unorm) ||
				!create_layer(dev, g_depth, w, h, format::r32_float) ||
				!create_layer(dev, g_overlay, w, h, format::r8g8b8a8_unorm))
			{
				destroy_layers(dev);
				return;
			}
			g_width = w;
			g_height = h;
			bind(runtime);
		}
		const uint8_t *base = g_view + kHeader + g_stride * slot;
		const size_t layer = size_t(w) * h * 4;
		subresource_data data;
		data.row_pitch = w * 4;
		data.slice_pitch = static_cast<uint32_t>(layer);
		data.data = const_cast<uint8_t *>(base);
		dev->update_texture_region(data, g_world.tex, 0);
		data.data = const_cast<uint8_t *>(base + layer);
		dev->update_texture_region(data, g_depth.tex, 0);
		data.data = const_cast<uint8_t *>(base + 2 * layer);
		dev->update_texture_region(data, g_overlay.tex, 0);
		if (read<int64_t>(desc) != seq)
			return; // Minecraft rewrote the slot mid-copy: show the next one instead
		g_lastPublish = published;
		g_mcNear = read<float>(desc + 32);
		g_mcFar = read<float>(desc + 36);
		g_mcFlags = read<int32_t>(desc + 44);
		g_mcPose.fov = read<float>(desc + 40);
		g_mcPose.yaw = read<float>(desc + 72);
		g_mcPose.pitch = read<float>(desc + 76);
		g_mcPose.roll = read<float>(desc + 80);
		g_mcPose.x = read<double>(desc + 48);
		g_mcPose.y = read<double>(desc + 56);
		g_mcPose.z = read<double>(desc + 64);
		g_mcPose.valid = true;
		g_mcSteve[0] = read<double>(desc + 104);
		g_mcSteve[1] = read<double>(desc + 112);
		g_mcSteve[2] = read<double>(desc + 120);
		g_mcSteveValid = g_mcSteve[0] != 0.0 || g_mcSteve[1] != 0.0 || g_mcSteve[2] != 0.0;
		g_hasFrame = true;
	}

	/// Camera-to-world rotation, as Minecraft builds it: rotationYXZ(pi - yaw, -pitch, roll) (camera looks down -z).
	void camera_rotation(const Pose &p, float m[3][3])
	{
		const float d2r = 3.14159265f / 180.0f;
		const float a = 3.14159265f - p.yaw * d2r, b = -p.pitch * d2r, c = p.roll * d2r;
		const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b), cc = std::cos(c), sc = std::sin(c);
		// Ry(a) * Rx(b) * Rz(c)
		const float ry[3][3] = {{ca, 0, sa}, {0, 1, 0}, {-sa, 0, ca}};
		const float rx[3][3] = {{1, 0, 0}, {0, cb, -sb}, {0, sb, cb}};
		const float rz[3][3] = {{cc, -sc, 0}, {sc, cc, 0}, {0, 0, 1}};
		float t[3][3] = {};
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				for (int k = 0; k < 3; ++k)
					t[i][j] += ry[i][k] * rx[k][j];
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
			{
				m[i][j] = 0;
				for (int k = 0; k < 3; ++k)
					m[i][j] += t[i][k] * rz[k][j];
			}
	}

	/// Rows of R_mc^T * R_host: turns a ray in GTA's camera space into Minecraft's camera space.
	void warp_matrix(const Pose &host, const Pose &mc, float out[3][3])
	{
		float rh[3][3], rm[3][3];
		camera_rotation(host, rh);
		camera_rotation(mc, rm);
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
			{
				out[i][j] = 0;
				for (int k = 0; k < 3; ++k)
					out[i][j] += rm[k][i] * rh[k][j];
			}
	}

	/// Reload MCPassthrough.fx when the file changes (ReShade doesn't watch it), so tweaks don't need a GTA restart.
	void watch_effect_file(effect_runtime *runtime)
	{
		static ULONGLONG next = 0;
		static FILETIME last = {};
		static wchar_t path[MAX_PATH] = {};
		if (GetTickCount64() < next)
			return;
		next = GetTickCount64() + 1000;
		if (path[0] == 0)
		{
			GetModuleFileNameW(nullptr, path, MAX_PATH);
			wchar_t *slash = wcsrchr(path, L'\\');
			if (slash)
				wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"reshade-shaders\\Shaders\\MCPassthrough.fx");
		}
		WIN32_FILE_ATTRIBUTE_DATA info;
		if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info))
			return;
		if (last.dwLowDateTime != 0 && CompareFileTime(&info.ftLastWriteTime, &last) != 0)
			runtime->reload_effect_next_frame(kEffect);
		last = info.ftLastWriteTime;
	}

	void on_present(effect_runtime *runtime)
	{
		watch_effect_file(runtime); // every frame, even when the effect failed to compile
	}

	void on_begin_effects(effect_runtime *runtime, command_list *, resource_view, resource_view)
	{
		uint32_t bw = 0, bh = 0;
		runtime->get_screenshot_width_and_height(&bw, &bh);
		g_bbWidth = bw;
		g_bbHeight = bh;
		// the script tells us every frame; GTA's pause menu stops it, and then the menu shows without Minecraft
		bool on = g_active && GetTickCount64() - g_activeAt.load() < 250 && open_mapping();
		if (on)
			upload(runtime);
		on = on && g_hasFrame;
		// The technique stays enabled (preset); McActive gates it, so GTA passes through untouched until a
		// Minecraft frame is here. (Toggling techniques from inside this callback crashes ReShade.)
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "McActive"); v.handle != 0)
			runtime->set_uniform_value_bool(v, on);
		if (!on)
			return;
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "McPlanes"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_mcNear, g_mcFar, float(g_mcFlags));
		// a scene's look overrides the preset's light matching and depth bias; the preset's values come back after
		auto look = [&](const char *name, float want, float &saved) {
			const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, name);
			if (v.handle == 0)
				return;
			if (want >= 0.0f)
			{
				if (saved < 0.0f)
					runtime->get_uniform_value_float(v, &saved, 1);
				runtime->set_uniform_value_float(v, want);
			}
			else if (saved >= 0.0f)
			{
				runtime->set_uniform_value_float(v, saved);
				saved = -1.0f;
			}
		};
		look("LightMatch", g_lookLight.load(), g_savedLight);
		look("DepthBias", g_lookBias.load(), g_savedBias);
		look("SlopeBias", g_lookSlope.load(), g_savedSlope);
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "Shake"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_shakeX.load(), g_shakeY.load(), g_shakeRoll.load());
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "PortalWarp"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_portalWarp.load());
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "HostPlanes"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_hostNear.load(), g_hostFar.load());
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "HudMask"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_maskX0.load(), g_maskY0.load(), g_maskX1.load(), g_maskY1.load());
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "McCursor"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_cursorX.load(), g_cursorY.load(), g_cursorOn.load());

		// Re-projection from Minecraft's pose to GTA's latest (extrapolated by the effect's PosePrediction frames).
		Pose host, prev;
		Steve steve;
		{
			std::lock_guard<std::mutex> lock(g_poseLock);
			steve = g_steve;
			const unsigned lag = unsigned(std::clamp(g_poseLag.load(), 0, 2));
			if (g_hostPoseCount > lag)
				host = g_hostPoses[(g_hostPoseCount - 1 - lag) & 3];
			if (g_hostPoseCount > lag + 1)
				prev = g_hostPoses[(g_hostPoseCount - 2 - lag) & 3];
		}
		float predict = 0.0f;
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "PosePrediction"); v.handle != 0)
			runtime->get_uniform_value_float(v, &predict, 1);
		const bool warp = host.valid && g_mcPose.valid && !g_cameraLocked;
		if (warp && prev.valid && predict != 0.0f)
		{
			auto delta = [](float a, float b) { float d = std::fmod(a - b + 540.0f, 360.0f) - 180.0f; return d; };
			host.yaw += delta(host.yaw, prev.yaw) * predict;
			host.pitch += (host.pitch - prev.pitch) * predict;
			host.roll += (host.roll - prev.roll) * predict;
			host.x += (host.x - prev.x) * predict;
			host.y += (host.y - prev.y) * predict;
			host.z += (host.z - prev.z) * predict;
		}
		float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
		float t[3] = {0, 0, 0}, ts[3] = {0, 0, 0};
		if (warp)
		{
			warp_matrix(host, g_mcPose, m);
			// T = R_mc^T (host position - Minecraft's camera position), in Minecraft's camera space
			float rm[3][3];
			camera_rotation(g_mcPose, rm);
			const float d[3] = {float(host.x - g_mcPose.x), float(host.y - g_mcPose.y), float(host.z - g_mcPose.z)};
			for (int i = 0; i < 3; ++i)
				t[i] = rm[0][i] * d[0] + rm[1][i] * d[1] + rm[2][i] * d[2];
			// Steve: less how far he moved since Minecraft drew him (a jump of metres is a teleport: none)
			float ds[3] = {d[0], d[1], d[2]};
			if (steve.valid && g_mcSteveValid)
			{
				const double m0 = steve.pos[0] - g_mcSteve[0], m1 = steve.pos[1] - g_mcSteve[1], m2 = steve.pos[2] - g_mcSteve[2];
				if (m0 * m0 + m1 * m1 + m2 * m2 < 16.0)
				{
					ds[0] -= float(m0);
					ds[1] -= float(m1);
					ds[2] -= float(m2);
				}
			}
			for (int i = 0; i < 3; ++i)
				ts[i] = rm[0][i] * ds[0] + rm[1][i] * ds[1] + rm[2][i] * ds[2];
		}
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "SteveT"); v.handle != 0)
			runtime->set_uniform_value_float(v, ts[0], ts[1], ts[2]);
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "SteveBox"); v.handle != 0)
		{
			if (steve.valid)
				runtime->set_uniform_value_float(v, steve.box[0], steve.box[1], steve.box[2], steve.box[3]);
			else
				runtime->set_uniform_value_float(v, 0.0f, 0.0f, 0.0f, 0.0f);
		}
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "SteveDepth"); v.handle != 0)
			runtime->set_uniform_value_float(v, steve.depth[0], steve.depth[1], steve.depth[2]);
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "SteveCarFar"); v.handle != 0)
			runtime->set_uniform_value_float(v, steve.valid && steve.seated ? steve.carFar : 0.0f);
		// where Minecraft drew Steve, in the camera it drew him with (his feet, and up): only his own pixels take his
		// motion, and his old place is never drawn elsewhere (that showed him twice, and moved the blocks by him with him)
		{
			float sm[4] = {0, 0, 0, 0}, su[4] = {0, 1, 0, 0};
			if (steve.valid && g_mcSteveValid && g_mcPose.valid)
			{
				float rm[3][3];
				camera_rotation(g_mcPose, rm);
				const float d[3] = {float(g_mcSteve[0] - g_mcPose.x), float(g_mcSteve[1] - g_mcPose.y), float(g_mcSteve[2] - g_mcPose.z)};
				for (int i = 0; i < 3; ++i)
				{
					sm[i] = rm[0][i] * d[0] + rm[1][i] * d[1] + rm[2][i] * d[2];
					su[i] = rm[1][i]; // (world up, (0, 1, 0), in that camera)
				}
				sm[3] = steve.height;
				su[3] = steve.seated ? 1.0f : 0.0f;
			}
			if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "SteveMc"); v.handle != 0)
				runtime->set_uniform_value_float(v, sm[0], sm[1], sm[2], sm[3]);
			if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "SteveUp"); v.handle != 0)
				runtime->set_uniform_value_float(v, su[0], su[1], su[2], su[3]);
		}
		const char *rows[3] = {"WarpRow0", "WarpRow1", "WarpRow2"};
		for (int i = 0; i < 3; ++i)
			if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, rows[i]); v.handle != 0)
				runtime->set_uniform_value_float(v, m[i][0], m[i][1], m[i][2]);
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "WarpT"); v.handle != 0)
			runtime->set_uniform_value_float(v, t[0], t[1], t[2]);
		const float d2r = 3.14159265f / 180.0f;
		const float tanHost = std::tan((warp ? host.fov : g_mcPose.fov) * d2r * 0.5f), tanMc = std::tan(g_mcPose.fov * d2r * 0.5f);
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "WarpTan"); v.handle != 0)
			runtime->set_uniform_value_float(v, tanHost, tanMc, float(g_width) / float(g_height));

		// the cells mined out of GTA's world: GTA's camera and Minecraft's (position from the cells' corner, and the
		// camera-to-world rotation's rows), so the effect finds where each pixel's surface is among them
		upload_dig(runtime);
		const bool dig = g_digOn && host.valid && g_mcPose.valid;
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "DigOn"); v.handle != 0)
			runtime->set_uniform_value_float(v, dig ? 1.0f : 0.0f);
		if (dig)
		{
			auto pose = [&](const Pose &p, const char *pos, const char *const rows[3]) {
				float r[3][3];
				camera_rotation(p, r);
				if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, pos); v.handle != 0)
					runtime->set_uniform_value_float(v, float(p.x - g_digShown[0]), float(p.y - g_digShown[1]), float(p.z - g_digShown[2]));
				for (int i = 0; i < 3; ++i)
					if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, rows[i]); v.handle != 0)
						runtime->set_uniform_value_float(v, r[i][0], r[i][1], r[i][2]);
			};
			const char *const hostRows[3] = {"DigHostRow0", "DigHostRow1", "DigHostRow2"};
			const char *const mcRows[3] = {"DigMcRow0", "DigMcRow1", "DigMcRow2"};
			pose(host, "DigHostPos", hostRows);
			pose(g_mcPose, "DigMcPos", mcRows);
		}
	}

	void on_reloaded_effects(effect_runtime *runtime)
	{
		if (g_width != 0)
			bind(runtime);
	}

	void on_destroy_effect_runtime(effect_runtime *runtime)
	{
		destroy_layers(runtime->get_device());
		device *dev = runtime->get_device();
		if (g_dig.srv.handle != 0)
			dev->destroy_resource_view(g_dig.srv);
		if (g_dig.tex.handle != 0)
			dev->destroy_resource(g_dig.tex);
		g_dig = Layer();
		std::lock_guard<std::mutex> lock(g_digLock);
		g_digChanged = !g_digCells.empty(); // (uploaded again to a new runtime's texture)
	}
}

namespace compositor
{
	bool try_register(void *module)
	{
		if (g_registered)
			return true;
		if (!reshade::register_addon(module))
			return false;
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
		reshade::register_event<reshade::addon_event::reshade_present>(on_present);
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
		g_registered = true;
		reshade::log::message(reshade::log::level::info, "MCPassthrough: registered");
		return true;
	}

	void unregister(void *module)
	{
		if (g_registered.exchange(false))
			reshade::unregister_addon(module);
	}

	void set_active(bool active)
	{
		g_active = active;
		g_activeAt = GetTickCount64();
	}

	void set_hud_mask(float x0, float y0, float x1, float y1)
	{
		g_maskX0 = x0;
		g_maskY0 = y0;
		g_maskX1 = x1;
		g_maskY1 = y1;
	}

	void set_cursor(float x, float y, bool visible)
	{
		g_cursorX = x;
		g_cursorY = y;
		g_cursorOn = visible ? 1.0f : 0.0f;
	}

	void set_host_planes(float near_clip, float far_clip)
	{
		g_hostNear = near_clip;
		g_hostFar = far_clip;
	}

	void backbuffer_size(int &width, int &height)
	{
		width = int(g_bbWidth.load());
		height = int(g_bbHeight.load());
	}

	void set_camera_locked(bool locked)
	{
		g_cameraLocked = locked;
	}

	void set_screen_fx(float shake_x, float shake_y, float shake_roll, float portal_warp)
	{
		g_shakeX = shake_x;
		g_shakeY = shake_y;
		g_shakeRoll = shake_roll;
		g_portalWarp = portal_warp;
	}

	void set_look(float light_match, float depth_bias, float slope_bias)
	{
		g_lookLight = light_match;
		g_lookBias = depth_bias;
		g_lookSlope = slope_bias;
	}

	void set_host_pose(float yaw, float pitch, float roll, float fov, double x, double y, double z)
	{
		std::lock_guard<std::mutex> lock(g_poseLock);
		g_hostPoses[g_hostPoseCount & 3] = {yaw, pitch, roll, fov, x, y, z, true};
		++g_hostPoseCount;
	}

	void set_steve(float x0, float y0, float x1, float y1, float near_d, float far_d, float glass_d, double x, double y, double z,
		float height, bool seated, float car_far)
	{
		std::lock_guard<std::mutex> lock(g_poseLock); // (all of it: the render thread reads it too)
		g_steve.height = height;
		g_steve.seated = seated;
		g_steve.carFar = car_far;
		g_steve.box[0] = x0;
		g_steve.box[1] = y0;
		g_steve.box[2] = x1;
		g_steve.box[3] = y1;
		g_steve.depth[0] = near_d;
		g_steve.depth[1] = far_d;
		g_steve.depth[2] = glass_d;
		g_steve.pos[0] = x;
		g_steve.pos[1] = y;
		g_steve.pos[2] = z;
		g_steve.valid = x1 > x0;
	}

	void set_pose_lag(int frames)
	{
		g_poseLag = frames;
	}

	void set_dig(int ox, int oy, int oz, const unsigned char *cells)
	{
		std::lock_guard<std::mutex> lock(g_digLock);
		if (cells == nullptr)
			g_digCells.clear();
		else
			g_digCells.assign(cells, cells + 512 * 512);
		g_digOrigin[0] = ox;
		g_digOrigin[1] = oy;
		g_digOrigin[2] = oz;
		g_digChanged = true;
	}
}
