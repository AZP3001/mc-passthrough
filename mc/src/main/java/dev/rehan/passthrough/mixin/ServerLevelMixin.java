package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.HostDig;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerLevel;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** How far a block is mined (sent to the other players): the piece of the host's world being mined shows its cracks. */
@Mixin(ServerLevel.class)
abstract class ServerLevelMixin {
	@Inject(method = "destroyBlockProgress(ILnet/minecraft/core/BlockPos;I)V", at = @At("HEAD"))
	private void passthrough$cracks(final int id, final BlockPos pos, final int progress, final CallbackInfo ci) {
		HostDig.progress((ServerLevel) (Object) this, pos, progress);
	}
}
