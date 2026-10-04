package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.HostWater;
import dev.rehan.passthrough.Passthrough;
import dev.rehan.passthrough.WorldBridge;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.FluidState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Every block change on the server (placed, broken, blown up) goes to the host, so it can mirror solid blocks. And the
 * host's own water (HostWater) is water to Minecraft wherever a cell is empty.
 */
@Mixin(Level.class)
abstract class LevelMixin {
	@Shadow
	public abstract BlockState getBlockState(BlockPos pos);

	@Inject(method = "setBlock(Lnet/minecraft/core/BlockPos;Lnet/minecraft/world/level/block/state/BlockState;II)Z", at = @At("RETURN"))
	private void passthrough$blockChanged(final BlockPos pos, final BlockState state, final int flags, final int limit, final CallbackInfoReturnable<Boolean> cir) {
		if (cir.getReturnValueZ() && (Object) this instanceof ServerLevel level) {
			WorldBridge.onBlockChanged(level, pos, state);
		}
	}

	@Inject(method = "getFluidState(Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/level/material/FluidState;", at = @At("RETURN"), cancellable = true)
	private void passthrough$hostWater(final BlockPos pos, final CallbackInfoReturnable<FluidState> cir) {
		if (!Passthrough.active || !cir.getReturnValue().isEmpty() || ((Level) (Object) this).dimension() != Level.OVERWORLD) {
			return;
		}

		FluidState water = HostWater.at(pos);
		if (water != null && this.getBlockState(pos).isAir()) {
			cir.setReturnValue(water);
		}
	}
}
