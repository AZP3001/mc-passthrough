package dev.rehan.passthrough.client.mixin;

import com.mojang.blaze3d.vertex.PoseStack;
import dev.rehan.passthrough.Passthrough;
import net.minecraft.client.renderer.ScreenEffectRenderer;
import net.minecraft.client.renderer.SubmitNodeCollector;
import net.minecraft.client.renderer.state.level.PlayerRenderState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Under the host's water the host draws its own underwater look: Minecraft's (a tinted texture over the whole screen)
 * would lie over the host's picture too.
 */
@Mixin(ScreenEffectRenderer.class)
abstract class ScreenEffectRendererMixin {
	@Inject(method = "submitWater", at = @At("HEAD"), cancellable = true)
	private static void passthrough$noWaterOverlay(final PlayerRenderState.WaterOverlay overlay, final PoseStack poseStack, final SubmitNodeCollector collector,
		final CallbackInfo ci) {
		if (Passthrough.active) {
			ci.cancel();
		}
	}
}
