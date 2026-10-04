package dev.rehan.passthrough.client.mixin;

import dev.rehan.passthrough.client.HostState;
import net.minecraft.client.Camera;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.extract.LevelExtractor;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Redirect;

/** In first person the player's own body is drawn too (his head hidden, SteveRig): looking down shows his arms and legs. */
@Mixin(LevelExtractor.class)
abstract class LevelExtractorMixin {
	@Redirect(method = "extractVisibleEntities", at = @At(value = "INVOKE", target = "Lnet/minecraft/client/Camera;isDetached()Z"))
	private boolean passthrough$ownBody(final Camera camera) {
		if (camera.isDetached()) {
			return true;
		}

		HostState.Pose p = HostState.frame();
		return p != null && camera.entity() == Minecraft.getInstance().player;
	}
}
