package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.Passthrough;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.entity.projectile.FireworkRocketEntity;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Redirect;

/**
 * A firework boosting Steve's elytra: four times the push and four times the speed it pushes to (Minecraft's rocket
 * pulls the flight toward 1.5 blocks a tick along the look, adding 0.1 a tick; here 6 and 0.4).
 */
@Mixin(FireworkRocketEntity.class)
abstract class FireworkRocketEntityMixin {
	@Redirect(method = "tick", at = @At(value = "INVOKE", target = "Lnet/minecraft/world/entity/LivingEntity;setDeltaMovement(Lnet/minecraft/world/phys/Vec3;)V"))
	private void passthrough$boost(final LivingEntity flyer, final Vec3 vanilla) {
		if (Passthrough.active && flyer instanceof Player && flyer.isFallFlying()) {
			// vanilla: m + look * 0.1 + (look * 1.5 - m) * 0.5 = m / 2 + look * 0.85; here m / 2 + look * 3.4
			flyer.setDeltaMovement(vanilla.add(flyer.getLookAngle().scale(2.55)));
			return;
		}

		flyer.setDeltaMovement(vanilla);
	}
}
