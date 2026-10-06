package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.Passthrough;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Steve's elytra, four times Minecraft's: every tick's change of speed (gravity, lift, the dive's pull, the turn) counts
 * four times, while the drag stays, so the top speed is four times too. A real fall (past a jump's height, 1.3 blocks) speeds up
 * as GTA's do (9.8 m/s per second, to about 52 m/s), not Minecraft's 32 to 78. The host's void doesn't kill the
 * player (the host puts it back up on its ground).
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

	@Unique
	private static final double GTA_GRAVITY = 9.8 / 400.0; // blocks per tick, per tick
	@Unique
	private static final double GTA_TOP_SPEED = 2.6; // blocks per tick

	@Inject(method = "getEffectiveGravity", at = @At("RETURN"), cancellable = true)
	private void passthrough$gtaFall(final CallbackInfoReturnable<Double> cir) {
		LivingEntity self = (LivingEntity) (Object) this;
		if (!Passthrough.active || !(self instanceof Player player) || player.getAbilities().flying || self.onGround() || self.isFallFlying()
			|| self.isInWater() || self.isInLava() || self.fallDistance < 1.3 || cir.getReturnValue() < 0.07) {
			return;
		}

		// (Minecraft then takes 2% off for drag: the gravity is what makes the speed after it come out as GTA's)
		double vy = self.getDeltaMovement().y;
		if (vy > 0.0) {
			return;
		}

		double want = Math.max(vy - GTA_GRAVITY, -GTA_TOP_SPEED);
		cir.setReturnValue(vy - want / 0.98);
	}

	@Inject(method = "onBelowWorld", at = @At("HEAD"), cancellable = true)
	private void passthrough$noVoid(final CallbackInfo ci) {
		if (Passthrough.active && (Object) this instanceof Player) {
			ci.cancel();
		}
	}

	/** Never dead in Minecraft while the host has the player's health: see HostBridge.overkill. */
	@Inject(method = "setHealth", at = @At("HEAD"), cancellable = true)
	private void passthrough$hostDecidesDeath(final float health, final CallbackInfo ci) {
		LivingEntity self = (LivingEntity) (Object) this;
		if (health >= 1.0F || !Passthrough.active || Passthrough.hostHealth < 0.0F || !(self instanceof net.minecraft.server.level.ServerPlayer player)
			|| player.isSpectator() || !player.isAlive()) {
			return;
		}

		dev.rehan.passthrough.HostBridge.overkill += 1.0F - Math.max(health, -1000.0F);
		ci.cancel();
		self.setHealth(1.0F);
	}
}
