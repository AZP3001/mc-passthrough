package dev.rehan.passthrough.client;

import net.minecraft.client.model.HumanoidModel;
import net.minecraft.client.model.geom.ModelPart;
import net.minecraft.client.renderer.entity.state.AvatarRenderState;
import org.joml.Vector3f;

/**
 * Steve posed as the host's character, while the host moves it (HostState.Pose.rig): the body turned and leaned as the
 * character's (by AvatarRendererMixin: its yaw, then a lean in setupRotations), and the head, arms and legs turned to
 * point as the character's do (by PlayerModelMixin, after Minecraft's own animation). So he walks, sits in a car's seat,
 * climbs, aims and lies as the character does.
 *
 * <p>The body's frame is (right R, up U, forward F = U x R). Minecraft renders the model turned by Ry(180 - bodyRot), then
 * our lean Rz(b) Rx(a): together their columns are exactly (R, U, -F). In model space +x is the character's left, +y
 * down, -z forward, so a world direction d is (-d.R, -d.U, -d.F) there.
 */
public final class SteveRig {
	/** The pose being drawn this frame, and the render state it belongs to (the local player's). Render thread only. */
	private static Pose current;
	private static AvatarRenderState currentState;
	/** The local player's render state this frame, how much bigger than Minecraft's Steve he's drawn, and whether the
	 * camera is in his head (first person: his body shows, his head doesn't). */
	private static AvatarRenderState localState;
	private static float localScale = 1.0F;
	private static boolean localFirstPerson;

	/** Angles in radians: body yaw (degrees, Minecraft's), the lean, the head's (x, y, z) and each limb's (x, z). */
	public record Pose(float bodyRot, float leanA, float leanB, float headX, float headY, float headZ,
		float leftArmX, float leftArmZ, float rightArmX, float rightArmZ, float leftLegX, float leftLegZ, float rightLegX, float rightLegZ) {
	}

	private SteveRig() {
	}

	/** The 24 numbers the host sends: body up, body right, head forward, head up, left arm, right arm, left leg, right leg. */
	public static Pose solve(final float[] v) {
		Vector3f up = new Vector3f(v[0], v[1], v[2]).normalize();
		Vector3f right = new Vector3f(v[3], v[4], v[5]);
		right.sub(new Vector3f(up).mul(right.dot(up))).normalize();
		Vector3f fwd = new Vector3f(up).cross(right);
		// body: Q = Ry(psi) Rz(b) Rx(a) with columns (R, U, -F)
		float b = (float) Math.asin(clamp(right.y));
		float a = (float) Math.atan2(fwd.y, up.y);
		float psi = (float) Math.atan2(-right.z, right.x);
		float bodyRot = 180.0F - (float) Math.toDegrees(psi);
		// head: the rotation taking the model's forward (0, 0, -1) and up (0, -1, 0) to the head's, as ZYX angles
		Vector3f hf = model(new Vector3f(v[6], v[7], v[8]), right, up, fwd).normalize();
		Vector3f hu = model(new Vector3f(v[9], v[10], v[11]), right, up, fwd);
		hu.sub(new Vector3f(hf).mul(hu.dot(hf))).normalize();
		Vector3f c2 = new Vector3f(hf).negate(), c1 = new Vector3f(hu).negate(), c0 = new Vector3f(hu).cross(hf);
		float headY = (float) Math.asin(clamp(-c0.z));
		float headX = (float) Math.atan2(c1.z, c2.z);
		float headZ = (float) Math.atan2(c0.y, c0.x);
		float[] la = limb(model(new Vector3f(v[12], v[13], v[14]), right, up, fwd));
		float[] ra = limb(model(new Vector3f(v[15], v[16], v[17]), right, up, fwd));
		float[] ll = limb(model(new Vector3f(v[18], v[19], v[20]), right, up, fwd));
		float[] rl = limb(model(new Vector3f(v[21], v[22], v[23]), right, up, fwd));
		return new Pose(bodyRot, a, b, headX, headY, headZ, la[0], la[1], ra[0], ra[1], ll[0], ll[1], rl[0], rl[1]);
	}

	/** A world direction in model space. */
	private static Vector3f model(final Vector3f d, final Vector3f right, final Vector3f up, final Vector3f fwd) {
		return new Vector3f(-d.dot(right), -d.dot(up), -d.dot(fwd));
	}

	/** The (x, z) turn that points a limb hanging down (model +y) along m: Rz(z) Rx(x) (0, 1, 0) = m. */
	private static float[] limb(final Vector3f m) {
		m.normalize();
		return new float[] {(float) Math.asin(clamp(m.z)), (float) Math.atan2(-m.x, m.y)};
	}

	private static float clamp(final float x) {
		return Math.max(-1.0F, Math.min(1.0F, x));
	}

	public static void set(final AvatarRenderState state, final Pose pose) {
		current = pose;
		currentState = state;
	}

	public static void setLocal(final AvatarRenderState state, final float scale, final boolean firstPerson) {
		localState = state;
		localScale = scale;
		localFirstPerson = firstPerson;
	}

	/** How much to scale this render state's model (the local player's: to the host character's height). */
	public static float scaleOf(final AvatarRenderState state) {
		return state == localState ? localScale : 1.0F;
	}

	/** Whether this render state is the local player's seen from inside his head. */
	public static boolean firstPerson(final AvatarRenderState state) {
		return state == localState && localFirstPerson;
	}

	/** The pose for this render state (the local player's this frame), or null. */
	public static Pose of(final AvatarRenderState state) {
		return state == currentState ? current : null;
	}

	/** After Minecraft's own animation: the head, arms (unless swinging, using or aiming something) and legs. */
	public static void apply(final HumanoidModel<AvatarRenderState> model, final AvatarRenderState state, final Pose p) {
		set(model.head, p.headX(), p.headY(), p.headZ());
		boolean armsBusy = state.swingAnimation > 0.0F || state.isUsingItem
			|| !free(state.rightArmPose) || !free(state.leftArmPose);
		if (!armsBusy) {
			set(model.leftArm, p.leftArmX(), 0.0F, p.leftArmZ());
			set(model.rightArm, p.rightArmX(), 0.0F, p.rightArmZ());
		}

		set(model.leftLeg, p.leftLegX(), 0.0F, p.leftLegZ());
		set(model.rightLeg, p.rightLegX(), 0.0F, p.rightLegZ());
		model.body.xRot = 0.0F;
		model.body.yRot = 0.0F;
		model.body.zRot = 0.0F;
	}

	private static boolean free(final HumanoidModel.ArmPose pose) {
		return pose == HumanoidModel.ArmPose.EMPTY || pose == HumanoidModel.ArmPose.ITEM;
	}

	private static void set(final ModelPart part, final float x, final float y, final float z) {
		part.xRot = x;
		part.yRot = y;
		part.zRot = z;
	}
}
