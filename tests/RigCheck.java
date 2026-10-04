// Checks SteveRig.solve (Steve posed from GTA bones) against known poses. Build the mod first (mc/build/classes), then:
//   CP=mc/build/classes/java/client:<minecraft-client.jar>:<joml.jar>; javac -cp $CP -d /tmp tests/RigCheck.java; java -cp /tmp:$CP RigCheck
import dev.rehan.passthrough.client.SteveRig;

// Known poses through SteveRig.solve (Minecraft coordinates: x east, y up, z south; GTA-side vectors converted).
public class RigCheck {
	static int fails = 0;
	static void check(boolean ok, String what) { System.out.println((ok ? "OK:   " : "FAIL: ") + what); if (!ok) fails++; }
	static boolean near(float a, double b) { return Math.abs(a - b) < 0.02; }
	public static void main(String[] a) {
		double R = Math.PI / 180;
		// standing facing north (-z): up +y, right +x (east); head forward -z, up +y; arms and legs down
		float[] stand = {0,1,0, 1,0,0, 0,0,-1, 0,1,0, 0,-1,0, 0,-1,0, 0,-1,0, 0,-1,0};
		SteveRig.Pose p = SteveRig.solve(stand);
		check(near(p.bodyRot(), 180) && near(p.leanA(), 0) && near(p.leanB(), 0), "standing north: body yaw 180, no lean " + p);
		check(near(p.headX(), 0) && near(p.headY(), 0) && near(p.headZ(), 0), "head straight");
		check(near(p.leftArmX(), 0) && near(p.leftArmZ(), 0) && near(p.rightLegX(), 0), "arms and legs hang straight");
		// facing south (+z): right is west (-x)
		float[] south = {0,1,0, -1,0,0, 0,0,1, 0,1,0, 0,-1,0, 0,-1,0, 0,-1,0, 0,-1,0};
		check(near(SteveRig.solve(south).bodyRot(), 0) || near(SteveRig.solve(south).bodyRot(), 360), "facing south: body yaw 0");
		// facing north, arms straight forward (zombie): Minecraft's arm xRot -90 deg
		float[] zombie = stand.clone();
		zombie[12] = 0; zombie[13] = 0; zombie[14] = -1; zombie[15] = 0; zombie[16] = 0; zombie[17] = -1;
		p = SteveRig.solve(zombie);
		check(near(p.leftArmX(), -90 * R) && near(p.rightArmX(), -90 * R), "arms forward: xRot -90 deg " + p.leftArmX());
		// sitting: thighs forward-down, legs xRot about -60..-90
		float[] sit = stand.clone();
		sit[18] = 0; sit[19] = -0.5f; sit[20] = -0.866f; sit[21] = 0; sit[22] = -0.5f; sit[23] = -0.866f;
		p = SteveRig.solve(sit);
		check(near(p.leftLegX(), -60 * R) && near(p.rightLegX(), -60 * R), "sitting, legs 60 deg forward: xRot -60 deg " + p.leftLegX());
		// right arm out to the right side (east when facing north): Minecraft's right arm zRot +90 deg
		float[] tpose = stand.clone();
		tpose[15] = 1; tpose[16] = 0; tpose[17] = 0;
		p = SteveRig.solve(tpose);
		check(near(p.rightArmZ(), 90 * R), "right arm out to the side: zRot +90 deg " + p.rightArmZ());
		// head looking down 30 deg: Minecraft's head xRot +30 deg
		float[] down = stand.clone();
		down[6] = 0; down[7] = (float) -Math.sin(30 * R); down[8] = (float) -Math.cos(30 * R);
		down[9] = 0; down[10] = (float) Math.cos(30 * R); down[11] = (float) -Math.sin(30 * R);
		p = SteveRig.solve(down);
		check(near(p.headX(), 30 * R) && near(p.headY(), 0), "head down 30 deg: xRot +30 deg " + p.headX());
		// head turned to look east (the body's right) while facing north: Minecraft's head yRot +90 deg
		float[] right = stand.clone();
		right[6] = 1; right[7] = 0; right[8] = 0;
		p = SteveRig.solve(right);
		check(near(p.headY(), 90 * R), "head turned to its right: yRot +90 deg " + p.headY());
		// lying on its back, head north: up is -z... body up points north, forward points up
		float[] lie = {0,0,-1, -1,0,0, 0,1,0, 0,0,-1, 0,0,1, 0,0,1, 0,0,1, 0,0,1}; // (face up, head north: the right hand is west)
		p = SteveRig.solve(lie);
		// the renderer's Ry(180 - bodyRot) Rz(b) Rx(a) applied to up (0,1,0) and to forward (0,0,-1) must give U and F
		double psi = Math.toRadians(180 - p.bodyRot()), b = p.leanB(), aa = p.leanA();
		double[] up = rot(psi, b, aa, new double[]{0,1,0}), fw = rot(psi, b, aa, new double[]{0,0,-1});
		check(Math.abs(up[2] + 1) < 0.02 && Math.abs(fw[1] - 1) < 0.02, String.format("lying on its back: model up %.2f %.2f %.2f, forward %.2f %.2f %.2f", up[0], up[1], up[2], fw[0], fw[1], fw[2]));
		check(near(p.leftLegX(), 0) && near(p.leftArmX(), 0), "and its limbs along its body");
		System.out.println(fails == 0 ? "ALL PASSED" : fails + " FAILED");
		System.exit(fails == 0 ? 0 : 1);
	}
	// Ry(psi) Rz(b) Rx(a) v
	static double[] rot(double psi, double b, double a, double[] v) {
		double[] x = {v[0], v[1] * Math.cos(a) - v[2] * Math.sin(a), v[1] * Math.sin(a) + v[2] * Math.cos(a)};
		double[] z = {x[0] * Math.cos(b) - x[1] * Math.sin(b), x[0] * Math.sin(b) + x[1] * Math.cos(b), x[2]};
		return new double[]{z[0] * Math.cos(psi) + z[2] * Math.sin(psi), z[1], -z[0] * Math.sin(psi) + z[2] * Math.cos(psi)};
	}
}
