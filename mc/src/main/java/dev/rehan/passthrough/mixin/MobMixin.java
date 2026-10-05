package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.Leads;
import dev.rehan.passthrough.Passthrough;
import net.minecraft.world.entity.Mob;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Minecraft's world is always day here (so its lighting stays bright): undead don't burn in it. And the host's leads
 * (Leads: their ends are invisible mobs) snap when the host says, not when Minecraft thinks them stretched too far.
 */
@Mixin(Mob.class)
abstract class MobMixin {
	@Inject(method = "burnUndead()V", at = @At("HEAD"), cancellable = true)
	private void passthrough$noSunburn(final CallbackInfo ci) {
		if (Passthrough.active) {
			ci.cancel();
		}
	}

	@Inject(method = "leashTooFarBehaviour()V", at = @At("HEAD"), cancellable = true)
	private void passthrough$hostLeads(final CallbackInfo ci) {
		if (Leads.isProxy((Mob) (Object) this)) {
			ci.cancel();
		}
	}
}
