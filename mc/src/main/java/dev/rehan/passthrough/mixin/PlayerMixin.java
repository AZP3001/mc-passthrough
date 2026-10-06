package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.Passthrough;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.phys.Vec3;
import org.objectweb.asm.Opcodes;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;
import net.minecraft.world.damagesource.DamageSource;

/**
 * While the host moves the player, neither side collides it with blocks: the host's ground arrives as barriers the
 * player can end up inside, and the server would otherwise reject those moves. While Minecraft moves the player
 * (Passthrough.walking), the client's player has its physics (against the host's real collision, HostCollision);
 * the server's still takes the client's word for where it is.
 */
@Mixin(Player.class)
abstract class PlayerMixin {
	@Inject(method = "tick", at = @At(value = "FIELD", target = "Lnet/minecraft/world/entity/player/Player;noPhysics:Z", opcode = Opcodes.PUTFIELD, shift = At.Shift.AFTER))
	private void passthrough$ghost(final CallbackInfo ci) {
		Entity self = (Entity) (Object) this;
		if (Passthrough.active && !(Passthrough.walking && self.level().isClientSide())) {
			self.noPhysics = true;
		}
	}

	/**
	 * The server's player doesn't move by itself (the client, or the host, says where it is): without collision it fell
	 * metres through the ground every tick before being put back, and the items it walked over were looked for down
	 * there, so none were picked up.
	 */
	@Inject(method = "travel", at = @At("HEAD"), cancellable = true)
	private void passthrough$noServerTravel(final Vec3 input, final CallbackInfo ci) {
		Entity self = (Entity) (Object) this;
		if (Passthrough.active && !self.level().isClientSide()) {
			ci.cancel();
		}
	}

	@Unique
	private boolean passthrough$mayflyWas;

	/**
	 * A fall hurts though the player may fly (/fly or creative gives it that): Minecraft spares anyone who may fly,
	 * so a fall never hurt here, and a parachute or a water bucket was never needed. Flying itself never falls.
	 */
	@Inject(method = "causeFallDamage", at = @At("HEAD"))
	private void passthrough$fallHurts(final double fallDistance, final float damageModifier, final DamageSource source,
		final CallbackInfoReturnable<Boolean> cir) {
		Player self = (Player) (Object) this;
		this.passthrough$mayflyWas = self.getAbilities().mayfly;
		if (Passthrough.active && !self.isSpectator() && !self.getAbilities().flying) {
			self.getAbilities().mayfly = false;
		}
	}

	@Inject(method = "causeFallDamage", at = @At("RETURN"))
	private void passthrough$fallHurtsAfter(final double fallDistance, final float damageModifier, final DamageSource source,
		final CallbackInfoReturnable<Boolean> cir) {
		((Player) (Object) this).getAbilities().mayfly = this.passthrough$mayflyWas;
	}
}
