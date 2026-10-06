package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.Passthrough;
import net.minecraft.world.entity.ai.goal.MeleeAttackGoal;
import net.minecraft.world.entity.player.Player;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Redirect;

/** A creative player is hunted all the same while a host is attached (the mobs' fight is the host's player's too). */
@Mixin(MeleeAttackGoal.class)
abstract class MeleeAttackGoalMixin {
	@Redirect(method = "canContinueToUse", at = @At(value = "INVOKE", target = "Lnet/minecraft/world/entity/player/Player;isCreative()Z"))
	private boolean passthrough$huntCreative(final Player player) {
		return !Passthrough.active && player.isCreative();
	}
}
