package dev.rehan.passthrough.client.mixin;

import dev.rehan.passthrough.Passthrough;
import dev.rehan.passthrough.client.PlayerSync;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.core.BlockPos;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(LocalPlayer.class)
abstract class LocalPlayerMixin {
	/** After the old position was saved for interpolation and before movement is sent to the server. */
	@Inject(method = "tick", at = @At("HEAD"))
	private void passthrough$followHost(final CallbackInfo ci) {
		PlayerSync.tick((LocalPlayer) (Object) this);
	}

	/**
	 * While Minecraft moves the player it isn't pushed out of "solid" cells: the host's collision boxes (a floor just
	 * re-probed a little higher, a pearl landing by a wall) would shove it sideways. Its collision lets it walk out.
	 */
	@Inject(method = "suffocatesAt", at = @At("HEAD"), cancellable = true)
	private void passthrough$noShove(final BlockPos pos, final CallbackInfoReturnable<Boolean> cir) {
		if (Passthrough.walking) {
			cir.setReturnValue(false);
		}
	}
}
