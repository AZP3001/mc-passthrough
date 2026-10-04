package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.Passthrough;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.player.Player;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** The host's rain is rain to the player too (a trident's riptide works in it), though Minecraft's own weather is clear. */
@Mixin(Entity.class)
abstract class EntityRainMixin {
	@Inject(method = "isInRain", at = @At("HEAD"), cancellable = true)
	private void passthrough$hostRain(final CallbackInfoReturnable<Boolean> cir) {
		if (Passthrough.active && Passthrough.hostRain && (Object) this instanceof Player) {
			cir.setReturnValue(true);
		}
	}
}
