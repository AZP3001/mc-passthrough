package dev.rehan.passthrough.client;

import dev.rehan.passthrough.Passthrough;
import java.util.Locale;
import net.minecraft.client.CameraType;
import net.minecraft.client.Minecraft;
import net.minecraft.client.Options;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.player.Abilities;
import net.minecraft.world.phys.Vec3;

/**
 * Keeps the Minecraft player and the host's player together. Usually the host moves its player and Minecraft's stands
 * where it stands; while walking (Minecraft's movement, on foot) Minecraft moves its player with its own physics and
 * keys, and the host's follows. Either way it looks where the host's camera looks.
 */
public final class PlayerSync {
	private static final double TELEPORT_SQ = 64.0 * 64.0;
	/** How far the host's player moved over the last client tick (drives the walk animation). */
	private static float tickDistance;
	private static double lastX = Double.NaN, lastZ;
	/** Minecraft moves the player (client thread). */
	private static boolean walking;
	/** The movement keys last set (only changes are applied: toggle-mode keys flip on every press). */
	private static int keysSet;
	/** The last position the host put the player at ("pset"), echoed so the host knows its correction arrived. */
	private static int psetApplied;
	/** Where the host's camera looks (Minecraft yaw), for walking that way. */
	private static volatile float cameraYaw;
	/** The host's car flies with the player (Minecraft's flight on for it). */
	private static boolean flyingCar;

	private PlayerSync() {
	}

	public static float tickDistance() {
		return tickDistance;
	}

	public static boolean walking() {
		return walking;
	}

	/** Every frame, before the camera update: position, rotation, and first/third person to match the host. */
	public static void frame(final float partialTick) {
		HostState.Pose p = HostState.frame();
		Minecraft minecraft = Minecraft.getInstance();
		LocalPlayer player = minecraft.player;
		if (p == null || player == null) {
			return;
		}

		if (p.drive()) {
			// Minecraft flies the player (elytra): it only takes where to look, and tells the host where it is
			player.setYRot(p.lookYaw());
			player.setXRot(p.lookPitch());
			player.yRotO = p.lookYaw();
			player.xRotO = p.lookPitch();
			player.yHeadRot = player.yHeadRotO = p.lookYaw();
			if (minecraft.options.getCameraType() != CameraType.THIRD_PERSON_BACK) {
				minecraft.options.setCameraType(CameraType.THIRD_PERSON_BACK);
			}
			// airborne: with no collision onGround never updates, and the server cancels a grounded player's glide
			player.setOnGround(false);
			sendPosition(player, partialTick, false);
			return;
		}

		// in third person Steve aims from his own eyes at the host's crosshair point (his head is beside the
		// camera's line: looking along it, his arrows and throws would miss the crosshair by that much)
		Vec3 feet = p.walk() && walking ? player.getPosition(partialTick) : new Vec3(p.px(), p.py(), p.pz());
		float yaw = p.yaw(), pitch = p.pitch();
		if (!p.firstPerson() && p.aim() != null) {
			double dx = p.aim()[0] - feet.x, dy = p.aim()[1] - (feet.y + player.getEyeHeight()), dz = p.aim()[2] - feet.z;
			double pr = Math.toRadians(p.pitch()), yr = Math.toRadians(p.yaw());
			double ahead = -dx * Math.sin(yr) * Math.cos(pr) - dy * Math.sin(pr) + dz * Math.cos(yr) * Math.cos(pr);
			if (ahead > 1.5) {
				yaw = (float) Math.toDegrees(Math.atan2(-dx, dz));
				pitch = (float) Math.toDegrees(-Math.atan2(dy, Math.sqrt(dx * dx + dz * dz)));
			}
		}

		player.setYRot(yaw);
		player.setXRot(pitch);
		player.yRotO = yaw;
		player.xRotO = pitch;
		player.yHeadRot = player.yHeadRotO = yaw;
		CameraType cameraType = p.firstPerson() ? CameraType.FIRST_PERSON : CameraType.THIRD_PERSON_BACK;
		if (minecraft.options.getCameraType() != cameraType) {
			minecraft.options.setCameraType(cameraType);
		}

		if (p.walk()) {
			// Minecraft moves the player (its body turns by itself, as it walks): the host's follows where it is
			if (walking) {
				sendPosition(player, partialTick, true);
			}

			return;
		}

		player.yBodyRot = player.yBodyRotO = p.firstPerson() ? p.yaw() : p.bodyYaw();
		// the model stands exactly where the host's player is this frame (not a tick behind, interpolating)
		double x = p.firstPerson() ? p.x() : p.px();
		double y = p.firstPerson() ? p.y() - player.getEyeHeight() : p.py();
		double z = p.firstPerson() ? p.z() : p.pz();
		player.setPos(x, y, z);
		player.xo = player.xOld = x;
		player.yo = player.yOld = y;
		player.zo = player.zOld = z;
	}

	/**
	 * Where the player is drawn this frame, how fast that moves (the slope of the tick interpolation, per second) and
	 * when (System.nanoTime is the host's QueryPerformanceCounter clock): the host carries it forward to its frame.
	 */
	private static void sendPosition(final LocalPlayer player, final float partialTick, final boolean walk) {
		Vec3 at = player.getPosition(partialTick);
		double vx = (player.getX() - player.xo) * 20.0, vy = (player.getY() - player.yo) * 20.0, vz = (player.getZ() - player.zo) * 20.0;
		Passthrough.events.accept(String.format(Locale.ROOT,
			"{\"t\":\"mcpos\",\"pos\":[%.4f,%.4f,%.4f],\"vel\":[%.3f,%.3f,%.3f],\"tn\":%d,\"fly\":%b,\"w\":%d,\"ps\":%d,\"g\":%d}",
			at.x, at.y, at.z, vx, vy, vz, System.nanoTime(), player.isFallFlying(), walk ? 1 : 0, psetApplied, player.onGround() ? 1 : 0));
	}

	/**
	 * Every client tick, at the start of the player's tick (the old position is already saved, so the model
	 * interpolates and walks): in first person the player's eyes are at the host camera, in third person
	 * their feet are at the host player's. While walking: the host's keys, and Minecraft's physics does the rest.
	 */
	public static void tick(final LocalPlayer player) {
		HostState.Pose p = HostState.live();
		Options options = Minecraft.getInstance().options;
		if (p != null && p.walk() && !p.drive()) {
			cameraYaw = p.yaw();
			if (!walking) {
				// Minecraft takes over where the host's player stands (it's there already: the host has put it there,
				// and puts it exactly there again, "pset"), from standing still, on its feet
				walking = true;
				Passthrough.walking = true;
				player.setDeltaMovement(Vec3.ZERO);
				Abilities abilities = player.getAbilities();
				if (abilities.flying && !player.isSpectator()) {
					abilities.flying = false; // (a spectator always flies)
					player.onUpdateAbilities();
				}
			}

			// a flying car: Minecraft's creative flight while it flies, its own walking and falling again after
			Abilities abilities = player.getAbilities();
			if (p.fly() != flyingCar) {
				flyingCar = p.fly();
				if (abilities.mayfly && !player.isSpectator()) {
					abilities.flying = flyingCar;
					player.onUpdateAbilities();
				}
			}

			keys(options, p.input());
			return;
		}

		if (walking) {
			walking = false;
			Passthrough.walking = false;
			keys(options, 0);
		}

		if (p == null || p.drive()) {
			return;
		}

		double x = p.firstPerson() ? p.x() : p.px();
		double y = p.firstPerson() ? p.y() - player.getEyeHeight() : p.py();
		double z = p.firstPerson() ? p.z() : p.pz();
		tickDistance = Double.isNaN(lastX) ? 0.0F : (float) Math.min(Math.hypot(x - lastX, z - lastZ), 1.0);
		lastX = x;
		lastZ = z;
		boolean teleport = player.distanceToSqr(x, y, z) > TELEPORT_SQ;
		player.setPos(x, y, z);
		if (teleport) {
			player.xo = player.xOld = x;
			player.yo = player.yOld = y;
			player.zo = player.zOld = z;
		}

		player.setDeltaMovement(Vec3.ZERO);
		Abilities abilities = player.getAbilities();
		if (abilities.mayfly && !abilities.flying) {
			abilities.flying = true;
			player.onUpdateAbilities();
		}
	}

	/** The yaw Minecraft's movement goes by: the host's camera while walking, else the player's own (`own`). */
	public static float moveYaw(final float own) {
		return walking ? cameraYaw : own;
	}

	/** Every movement key let go (the host's cutscene begins). */
	static void releaseKeys(final Options o) {
		keysSet = 0;
		for (net.minecraft.client.KeyMapping key : new net.minecraft.client.KeyMapping[]{o.keyUp, o.keyDown, o.keyLeft, o.keyRight, o.keyJump, o.keyShift, o.keySprint}) {
			key.setDown(false);
		}
	}

	/** The host's movement keys as Minecraft's own (only changes: a toggle-mode key flips on every press). */
	private static void keys(final Options o, final int in) {
		int changed = in ^ keysSet;
		keysSet = in;
		if ((changed & 1) != 0) {
			o.keyUp.setDown((in & 1) != 0);
		}

		if ((changed & 2) != 0) {
			o.keyDown.setDown((in & 2) != 0);
		}

		if ((changed & 4) != 0) {
			o.keyLeft.setDown((in & 4) != 0);
		}

		if ((changed & 8) != 0) {
			o.keyRight.setDown((in & 8) != 0);
		}

		if ((changed & 16) != 0) {
			o.keyJump.setDown((in & 16) != 0);
		}

		if ((changed & 32) != 0) {
			o.keyShift.setDown((in & 32) != 0);
		}

		if ((changed & 64) != 0) {
			o.keySprint.setDown((in & 64) != 0);
		}
	}

	/**
	 * The host put the player somewhere ({"t":"pset","id":n,"pos":[x,y,z],"keepY":bool}): it went through one of the
	 * host's walls before Minecraft knew of it, the host's own player was moved (a mission), or Minecraft's fell out of
	 * the host's world. keepY: only across (the player keeps its height and its fall). Client thread.
	 */
	static void pset(final LocalPlayer player, final int id, final double x, final double y, final double z, final boolean keepY) {
		psetApplied = id;
		double ny = keepY ? player.getY() : y;
		player.setPos(x, ny, z);
		player.xo = player.xOld = x;
		player.yo = player.yOld = ny;
		player.zo = player.zOld = z;
		Vec3 v = player.getDeltaMovement();
		player.setDeltaMovement(0.0, keepY ? v.y : 0.0, 0.0);
	}
}
