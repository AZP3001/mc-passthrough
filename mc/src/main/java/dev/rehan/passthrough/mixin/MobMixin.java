package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.Leads;
import dev.rehan.passthrough.Passthrough;
import net.minecraft.world.entity.Mob;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.Redirect;
import net.minecraft.world.entity.player.Player;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Minecraft's world is always day here (so its lighting stays bright): undead don't burn in it. And the host's leads
 * (Leads: their ends are invisible mobs) snap when the host says, not when Minecraft thinks them stretched too far; and
 * Minecraft's own leads never snap while a host is attached (the mob is brought to the one holding it).
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
		Mob self = (Mob) (Object) this;
		if (Leads.isProxy(self)) {
			ci.cancel();
		} else if (Passthrough.active && self.getLeashHolder() != null) {
			// leads never snap here: too far (the player teleported, a pearl), the mob is brought along beside the holder
			net.minecraft.world.entity.Entity holder = self.getLeashHolder();
			net.minecraft.world.phys.Vec3 away = self.position().subtract(holder.position());
			net.minecraft.world.phys.Vec3 side = away.horizontalDistanceSqr() > 1.0E-4 ? new net.minecraft.world.phys.Vec3(away.x, 0.0, away.z).normalize().scale(2.0)
				: new net.minecraft.world.phys.Vec3(2.0, 0.0, 0.0);
			self.snapTo(holder.getX() + side.x, holder.getY(), holder.getZ() + side.z, self.getYRot(), self.getXRot());
			self.setDeltaMovement(net.minecraft.world.phys.Vec3.ZERO);
			ci.cancel();
		}
	}

	/** A creative player is a target all the same while a host is attached (MeleeAttackGoalMixin: and stays one). */
	@Redirect(method = "asValidTarget", at = @At(value = "INVOKE", target = "Lnet/minecraft/world/entity/player/Player;isCreative()Z"))
	private boolean passthrough$creativeTarget(final Player player) {
		return !Passthrough.active && player.isCreative();
	}
}
