package dev.rehan.passthrough.client.mixin;

import dev.rehan.passthrough.Leads;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.PositionPath;
import org.jspecify.annotations.Nullable;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * The ends of the host's leads (Leads' proxies) are put where the host says every frame (HostLink): the server's
 * slightly older positions, eased toward, would make the lead wobble. And they're drawn (their lead is) from as far
 * away as the host's things are seen, not just from the few blocks a little allay is.
 */
@Mixin(Entity.class)
abstract class LeadProxyMixin {
	@Inject(method = "moveOrInterpolateTo(Lnet/minecraft/world/entity/PositionPath;FFZ)V", at = @At("HEAD"), cancellable = true)
	private void passthrough$hostPlaces(final @Nullable PositionPath position, final float yRot, final float xRot, final boolean hasRotation,
		final CallbackInfo ci) {
		Entity self = (Entity) (Object) this;
		if (self.level().isClientSide() && Leads.isProxy(self) && !self.isPassenger()) {
			ci.cancel();
		}
	}

	@Inject(method = "shouldRenderAtSqrDistance(D)Z", at = @At("HEAD"), cancellable = true)
	private void passthrough$seenFar(final double distance, final CallbackInfoReturnable<Boolean> cir) {
		if (Leads.isProxy((Entity) (Object) this)) {
			cir.setReturnValue(distance < 160.0 * 160.0);
		}
	}
}
