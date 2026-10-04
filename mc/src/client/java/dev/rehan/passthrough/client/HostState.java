package dev.rehan.passthrough.client;

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import dev.rehan.passthrough.Passthrough;
import net.minecraft.client.Minecraft;

/** The host's latest camera and player pose, already in Minecraft coordinates (the host converts). */
public final class HostState {
	/**
	 * @param hostFrame the host's frame number for this pose (echoed in the exported frame)
	 * @param x camera position
	 * @param yaw Minecraft yaw (0 = facing +Z), pitch (positive = down) and roll, in degrees
	 * @param fov vertical field of view, degrees
	 * @param firstPerson whether the host camera is first person (third person shows the player model)
	 * @param px the player's feet (third person)
	 * @param bodyYaw the player's body yaw (third person)
	 * @param drive Minecraft moves the player (elytra flight) and the host follows; lookYaw/lookPitch steer
	 * @param gun the player holds one of the host's guns (Steve aims it; his own item isn't drawn)
	 * @param vehicle the host's player sits in a vehicle (Steve sits too)
	 * @param sneak the host's player sneaks (stealth: Steve crouches)
	 * @param aim where the host's crosshair points (Steve shoots and throws there), or null
	 * @param walk Minecraft moves the player on foot (its physics, against the host's collision) and the host's player
	 *     follows it; px/py/pz is where the host's player stands (where Minecraft's starts from)
	 * @param input the movement keys while walking: 1 forward, 2 back, 4 left, 8 right, 16 jump, 32 sneak, 64 sprint
	 * @param dead the host's player is dead (Steve lies down)
	 * @param hostHit what the host's crosshair hits within reach: point and surface normal {x, y, z, nx, ny, nz}, or null
	 * @param control whether the player has control (false in the host's cutscenes, loading screens and scripted scenes:
	 *     then Minecraft takes no input at all)
	 * @param rig the host character's pose while the host moves it (SteveRig): unit vectors for the body's up and right,
	 *     the head's forward and up, and the left arm, right arm, left leg and right leg, or null
	 * @param health the host's player's health (and its most), armour (0..100) and stamina (0..100): Minecraft's hearts,
	 *     armour and hunger bars show them; health -1 when not sent
	 * @param fly Minecraft flies the player (a flying car): creative flight on
	 * @param height the host character's height in metres (soles to the top of the head): Steve is drawn that tall
	 */
	public record Pose(
		long hostFrame, double x, double y, double z, float yaw, float pitch, float roll, float fov,
		boolean firstPerson, double px, double py, double pz, float bodyYaw, long receivedNanos,
		boolean drive, float lookYaw, float lookPitch, boolean gun, boolean vehicle, boolean sneak, double[] aim,
		boolean walk, int input, boolean dead, double[] hostHit, boolean control, float[] rig,
		int health, int healthMax, int armor, float stamina, boolean fly, float height
	) {
	}

	private static final long TIMEOUT_NANOS = 2_000_000_000L;
	private static volatile Pose latest;
	private static int lastHealth = -1;
	/** Whether the last pose gave the player control (a change to false lets go of everything held). */
	private static volatile boolean hadControl = true;
	/** The pose this frame renders with, taken once per frame so every hook agrees. Render thread only. */
	private static Pose frame;

	private HostState() {
	}

	/** {"t":"cam","f":frame,"p":[x,y,z],"r":[yaw,pitch,roll],"fov":deg,"fp":bool,"pl":[x,y,z],"h":bodyYaw} */
	static void update(final JsonObject m) {
		JsonArray p = m.getAsJsonArray("p");
		JsonArray r = m.getAsJsonArray("r");
		JsonArray pl = m.has("pl") ? m.getAsJsonArray("pl") : p;
		float yaw = r.get(0).getAsFloat();
		Pose pose = new Pose(
			m.has("f") ? m.get("f").getAsLong() : 0L,
			p.get(0).getAsDouble(), p.get(1).getAsDouble(), p.get(2).getAsDouble(),
			yaw, r.get(1).getAsFloat(), r.size() > 2 ? r.get(2).getAsFloat() : 0.0F,
			m.has("fov") ? m.get("fov").getAsFloat() : 70.0F,
			!m.has("fp") || m.get("fp").getAsBoolean(),
			pl.get(0).getAsDouble(), pl.get(1).getAsDouble(), pl.get(2).getAsDouble(),
			m.has("h") ? m.get("h").getAsFloat() : yaw,
			System.nanoTime(),
			m.has("drive") && m.get("drive").getAsBoolean(),
			m.has("look") ? m.getAsJsonArray("look").get(0).getAsFloat() : yaw,
			m.has("look") ? m.getAsJsonArray("look").get(1).getAsFloat() : r.get(1).getAsFloat(),
			m.has("gun") && m.get("gun").getAsBoolean(),
			m.has("veh") && m.get("veh").getAsBoolean(),
			m.has("sn") && m.get("sn").getAsBoolean(),
			m.has("aimOn") && m.get("aimOn").getAsBoolean() && m.has("aim") ? doubles(m.getAsJsonArray("aim")) : null,
			m.has("walk") && m.get("walk").getAsBoolean(),
			m.has("in") ? m.get("in").getAsInt() : 0,
			m.has("dead") && m.get("dead").getAsBoolean(),
			m.has("ghOn") && m.get("ghOn").getAsBoolean() && m.has("gh") ? doubles(m.getAsJsonArray("gh")) : null,
			!m.has("ctl") || m.get("ctl").getAsBoolean(),
			m.has("rig") ? floats(m.getAsJsonArray("rig")) : null,
			m.has("hp") ? m.getAsJsonArray("hp").get(0).getAsInt() : -1,
			m.has("hp") ? Math.max(1, m.getAsJsonArray("hp").get(1).getAsInt()) : 1,
			m.has("ar") ? m.get("ar").getAsInt() : 0,
			m.has("st") ? m.get("st").getAsFloat() : 100.0F,
			m.has("fly") && m.get("fly").getAsBoolean(),
			m.has("ht") ? Math.max(1.4F, Math.min(2.1F, m.get("ht").getAsFloat())) : 1.875F
		);
		// the host's player hurt (shot, run over, a fall): Steve flinches red and Minecraft's hurt sound plays
		if (pose.health() >= 0 && lastHealth >= 0 && pose.health() < lastHealth - Math.max(2, pose.healthMax() / 40)) {
			Minecraft minecraft = Minecraft.getInstance();
			minecraft.execute(() -> {
				if (minecraft.player != null && minecraft.player.hurtTime == 0) {
					minecraft.player.hurtDuration = 10;
					minecraft.player.hurtTime = 10;
					minecraft.player.playSound(net.minecraft.sounds.SoundEvents.PLAYER_HURT, 0.8F, 1.0F);
				}
			});
		}

		lastHealth = pose.health();
		Passthrough.hostRain = m.has("rain") && m.get("rain").getAsFloat() > 0.15F;
		latest = pose;
		Passthrough.hostHit = pose.hostHit();
		Passthrough.hostHealth = pose.health() < 0 ? -1.0F : Math.min(20.0F, 20.0F * pose.health() / pose.healthMax());
		if (!pose.control() && hadControl) {
			// a cutscene (or a loading screen, a mission's scene) begins: nothing stays held
			Minecraft minecraft = Minecraft.getInstance();
			minecraft.execute(() -> ClientInput.releaseAll(minecraft));
		}

		hadControl = pose.control();
	}

	static float[] floats(final JsonArray a) {
		float[] out = new float[a.size()];
		for (int i = 0; i < out.length; i++) {
			out[i] = a.get(i).getAsFloat();
		}

		return out;
	}

	/** Whether the player may give Minecraft input now (no host, or the host says so). */
	static boolean control() {
		Pose p = live();
		return p == null || p.control();
	}

	static double[] doubles(final JsonArray a) {
		double[] out = new double[a.size()];
		for (int i = 0; i < out.length; i++) {
			out[i] = a.get(i).getAsDouble();
		}

		return out;
	}

	/** The latest pose if the host is still sending, else null. */
	static Pose live() {
		Pose p = latest;
		return p != null && System.nanoTime() - p.receivedNanos() < TIMEOUT_NANOS ? p : null;
	}

	/** The pose for the frame being rendered, or null when no host is attached. */
	public static Pose frame() {
		return frame;
	}

	public static void beginFrame() {
		frame = live();
		Passthrough.active = frame != null;
	}
}
