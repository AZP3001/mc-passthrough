package dev.rehan.passthrough.client;

/**
 * Minecraft's hearts, armour and hunger bars show the host's player's health, armour and stamina (sprinting tires it,
 * as hunger would), in creative too.
 */
public final class HostBars {
	private HostBars() {
	}

	/** Whether the bars show the host's (it sends them). */
	public static boolean shown() {
		HostState.Pose p = HostState.frame();
		return p != null && p.health() >= 0;
	}

	public static float health(final float own) {
		HostState.Pose p = HostState.frame();
		return p == null || p.health() < 0 ? own : Math.max(0.0F, Math.min(20.0F, 20.0F * p.health() / p.healthMax()));
	}

	public static int armor(final int own) {
		HostState.Pose p = HostState.frame();
		// GTA's body armour, or Minecraft's worn armour if that's more (both protect the player)
		return p == null || p.health() < 0 ? own : Math.max(own, Math.max(0, Math.min(20, Math.round(p.armor() * 20.0F / 100.0F))));
	}

	public static int food(final int own) {
		HostState.Pose p = HostState.frame();
		return p == null || p.health() < 0 ? own : Math.max(0, Math.min(20, Math.round(p.stamina() * 20.0F / 100.0F)));
	}
}
