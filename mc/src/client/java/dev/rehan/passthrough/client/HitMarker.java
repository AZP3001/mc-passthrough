package dev.rehan.passthrough.client;

import dev.rehan.passthrough.Passthrough;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.sounds.SoundEvents;

/**
 * A hit on the host's people or cars (Minecraft's swing, arrow or trident landed): a marker round the crosshair for a
 * moment (white, red for a kill) and Minecraft's sound for it (an arrow's ding, a strong swing's thud, the XP chime on
 * a kill). The host says when ({"t":"hitmark","kill":bool,"shot":bool}).
 */
public final class HitMarker {
	private static volatile long until;
	private static volatile long length = 1;
	private static volatile boolean kill;

	private HitMarker() {
	}

	/** Any thread. */
	public static void hit(final boolean killed, final boolean shot) {
		long now = System.nanoTime();
		length = (killed ? 450L : 250L) * 1_000_000L;
		until = now + length;
		kill = killed;
		Minecraft minecraft = Minecraft.getInstance();
		minecraft.execute(() -> {
			if (minecraft.player == null) {
				return;
			}

			if (killed) {
				minecraft.player.playSound(SoundEvents.EXPERIENCE_ORB_PICKUP, 0.6F, 0.9F);
			} else if (shot) {
				minecraft.player.playSound(SoundEvents.ARROW_HIT_PLAYER, 0.5F, 1.0F);
			} else {
				minecraft.player.playSound(SoundEvents.PLAYER_ATTACK_STRONG, 0.7F, 1.0F);
			}
		});
	}

	/** Every HUD frame: the marker while it lasts, fading out, round the middle of the screen (where the host's crosshair is). */
	public static void extract(final GuiGraphicsExtractor graphics) {
		long left = until - System.nanoTime();
		if (left <= 0 || !Passthrough.active) {
			return;
		}

		float fade = Math.min(1.0F, (float) left / (float) length * 1.6F);
		int alpha = Math.round(fade * 230.0F) << 24;
		int color = alpha | (kill ? 0xE03030 : 0xFFFFFF);
		int cx = graphics.guiWidth() / 2, cy = graphics.guiHeight() / 2;
		graphics.nextStratum();
		// four short diagonal strokes, a gap in the middle (the crosshair stays clear)
		for (int i = 3; i <= 6; i++) {
			graphics.fill(cx + i, cy + i, cx + i + 1, cy + i + 1, color);
			graphics.fill(cx - i - 1, cy + i, cx - i, cy + i + 1, color);
			graphics.fill(cx + i, cy - i - 1, cx + i + 1, cy - i, color);
			graphics.fill(cx - i - 1, cy - i - 1, cx - i, cy - i, color);
		}
	}
}
