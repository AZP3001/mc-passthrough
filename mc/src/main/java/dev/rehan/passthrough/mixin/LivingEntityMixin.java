package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.Passthrough;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Steve's elytra, four times Minecraft's: every tick's change of speed (gravity, lift, the dive's pull, the turn) counts
 * four times, while the drag stays, so the top speed is four times too.
 */
@Mixin(LivingEntity.class)
abstract class LivingEntityMixin {
	@Unique
	private static final double GLIDE = 4.0;
	@Unique
	private Vec3 passthrough$glideFrom = Vec3.ZERO;

	@Inject(method = "updateFallFlyingMovement", at = @At("HEAD"))
	private void passthrough$glideStart(final Vec3 movement, final CallbackInfoReturnable<Vec3> cir) {
		this.passthrough$glideFrom = movement;
	}

	@Inject(method = "updateFallFlyingMovement", at = @At("RETURN"), cancellable = true)
	private void passthrough$glideFaster(final Vec3 movement, final CallbackInfoReturnable<Vec3> cir) {
		if (!Passthrough.active || !((Object) this instanceof Player)) {
			return;
		}

		// vanilla: (v + change) * drag; here (v + change * 4) * drag
		Vec3 v = this.passthrough$glideFrom, out = cir.getReturnValue();
		Vec3 kept = v.multiply(0.99F, 0.98F, 0.99F);
		cir.setReturnValue(kept.add(out.subtract(kept).scale(GLIDE)));
	}
}
