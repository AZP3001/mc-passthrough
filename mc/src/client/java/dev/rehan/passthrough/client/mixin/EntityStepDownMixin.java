package dev.rehan.passthrough.client.mixin;

import dev.rehan.passthrough.HostCollision;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.MoverType;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Walking down the host's slopes, the player stays on the ground. The host's ground is 0.5 m floor cells, a staircase on
 * a slope: Minecraft steps up them, but walked off each one going down, and fell to the next (in the air most of the way
 * down a hill: no jumping, a bump at every cell). Now a walk off a cell onto one at most a step lower (0.6) goes down onto
 * it, as a step up goes up. A jump, flight, swimming, a ladder and a real drop (more than a step) are Minecraft's own.
 */
@Mixin(Entity.class)
abstract class EntityStepDownMixin {
	@Unique
	private boolean passthrough$wasOnGround;

	@Shadow
	private Vec3 collide(final Vec3 movement) {
		throw new AssertionError();
	}

	@Inject(method = "move", at = @At("HEAD"))
	private void passthrough$beforeMove(final MoverType type, final Vec3 delta, final CallbackInfo ci) {
		this.passthrough$wasOnGround = ((Entity) (Object) this).onGround();
	}

	@Inject(method = "move", at = @At("TAIL"))
	private void passthrough$stepDown(final MoverType type, final Vec3 delta, final CallbackInfo ci) {
		Entity self = (Entity) (Object) this;
		if (type != MoverType.SELF || !this.passthrough$wasOnGround || delta.y > 0.0 || !(self instanceof LocalPlayer player)
			|| !HostCollision.appliesTo(self) || player.getAbilities().flying || player.isFallFlying() || player.isInWater() || player.isInLava()
			|| player.onClimbable() || player.isPassenger() || player.getDeltaMovement().y > 0.0) {
			return;
		}

		// (whether or not Minecraft says it's on the ground: it moves up and down first, then across, so the tick it
		// walks off a cell it still stood on it, and went over the gap a tick late, by then more than a step above the
		// next cell)
		double step = player.maxUpStep();
		Vec3 down = this.collide(new Vec3(0.0, -step, 0.0));
		if (down.y >= -1.0E-4 || down.y <= -step + 1.0E-7) {
			return; // (on the ground, or nothing within a step under it: a real drop, fallen from as ever)
		}

		player.setPos(player.getX(), player.getY() + down.y, player.getZ());
		player.setOnGroundWithMovement(true, down);
		player.verticalCollision = true;
		player.verticalCollisionBelow = true;
		player.resetFallDistance();
		Vec3 v = player.getDeltaMovement();
		player.setDeltaMovement(v.x, 0.0, v.z);
	}
}
