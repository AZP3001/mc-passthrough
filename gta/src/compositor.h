// ReShade add-on half: uploads Minecraft's latest frame (shared memory "Local\MCPassthroughFrame") into textures
// that MCPassthrough.fx composites into GTA's picture against GTA's depth buffer.
#pragma once

namespace compositor
{
	/// Registers with ReShade once it is loaded; call until it returns true.
	bool try_register(void *module);
	void unregister(void *module);
	/// Whether to composite this frame (the script turns it off in menus and cutscenes). Call it every frame: when
	/// the calls stop (GTA's pause menu stops scripts), compositing stops too, so no old frame stays over the map.
	void set_active(bool active);
	/// A screen rectangle (0..1, x0 y0 top-left, x1 y1 bottom-right) that Minecraft's world never covers: GTA's
	/// minimap. x1 <= x0 for none.
	void set_hud_mask(float x0, float y0, float x1, float y1);
	/// The mouse pointer over Minecraft's screens (inventory, chat): GTA's own pointer is under Minecraft's overlay.
	void set_cursor(float x, float y, bool visible);
	/// GTA's camera clip planes, to turn its depth buffer into metres.
	void set_host_planes(float near_clip, float far_clip);
	/// GTA's current camera in Minecraft's convention (rotation in degrees, vertical fov, position in Minecraft
	/// coordinates): Minecraft's frame is re-projected (with its depth) from the pose it was rendered with to this
	/// one, hiding the link's latency on camera turns and moves.
	void set_host_pose(float yaw, float pitch, float roll, float fov, double x, double y, double z);
	/// How many poses back the presented picture is (1: the script reads the camera of the frame being prepared).
	void set_pose_lag(int frames);
	/// A cutscene plays: Steve (standing in for the player's character) is lit at least as GTA lights its characters.
	void set_cutscene(bool on);
	/// Minecraft's picture moves with the camera (the flight chase cam frames Steve): composite it as rendered,
	/// without re-projecting it to GTA's newer pose, so Steve stays exactly where the camera framed him.
	void set_camera_locked(bool locked);
	/// A scene's look: the effect's "Match GTA lighting", depth bias and slope bias (negative: the preset's own values).
	void set_look(float light_match, float depth_bias, float slope_bias);
	/// The camera shake, applied to the finished picture (x, y: fraction of the screen height; roll: radians), and the
	/// nether portal's warp (0..1).
	void set_screen_fx(float shake_x, float shake_y, float shake_roll, float portal_warp);
	/// Steve this frame: his screen box (0..1, x1 <= x0 for none), his depth range along the camera and the distance of
	/// glass in front of him (0: none), in metres, and where the character is now (Minecraft coordinates, feet): his part
	/// of Minecraft's picture is re-projected by the camera's motion less his own, so he stays on the character.
	/// height: his (metres); seated: in a car's seat (above his waist he shows through the car's windows, an open door's).
	/// car_far: seated, how far the car's own body reaches from the camera (only that close does it let him show through).
	void set_steve(float x0, float y0, float x1, float y1, float near_d, float far_d, float glass_d, double x, double y, double z,
		float height, bool seated, float car_far = 0.0f);
	/// Glass in front of Steve: box, the screen box (0..1) of a 5 x 8 grid over him; cells (row by row from the top), the
	/// distance along the camera of the glass a line to him meets first in that cell (0: none, or something solid first).
	void set_steve_glass(const float box[4], const float cells[40]);
	/// GTA's backbuffer size as ReShade sees it (0 until the first frame).
	void backbuffer_size(int &width, int &height);
}
