package dev.rehan.passthrough.client.mixin;

import dev.rehan.passthrough.client.HostState;
import dev.rehan.passthrough.client.PlayerSync;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.LivingEntity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.Redirect;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * The local player is placed every frame while the host moves it, so its own tick movement is ~0: animate the walk from
 * the host's movement. (While Minecraft moves it, its own walk animation is right.)
 */
@Mixin(LivingEntity.class)
abstract class LivingEntityMixin {
	@Inject(method = "updateWalkAnimation", at = @At("HEAD"), cancellable = true)
	private void passthrough$hostWalk(final float distance, final CallbackInfo ci) {
		if ((Object) this instanceof LocalPlayer player && HostState.frame() != null && !PlayerSync.walking()) {
			player.walkAnimation.update(Math.min(PlayerSync.tickDistance() * 4.0F, 1.0F), 0.4F, 1.0F);
			ci.cancel();
		}
	}

	/** A sprint jump leaps where the host's camera looks (see EntityPickMixin's moveRelative). */
	@Redirect(method = "jumpFromGround", at = @At(value = "INVOKE", target = "Lnet/minecraft/world/entity/LivingEntity;getYRot()F"))
	private float passthrough$leapAlongCamera(final LivingEntity self) {
		return self instanceof LocalPlayer ? PlayerSync.moveYaw(self.getYRot()) : self.getYRot();
	}
}
