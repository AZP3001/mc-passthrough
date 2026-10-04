package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.Passthrough;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.BarrierBlock;
import net.minecraft.world.level.block.SimpleWaterloggedBlock;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.Fluid;
import net.minecraft.world.level.material.Fluids;
import org.jspecify.annotations.Nullable;
import org.spongepowered.asm.mixin.Mixin;

/**
 * The host's ground is barriers, and barriers take water in (waterlogged): a bucket emptied on the host's street filled
 * the barrier under it, below the street, where the host's ground hid it. While a host is attached they don't, so the
 * water goes on top, where it was poured.
 */
@Mixin(BarrierBlock.class)
abstract class BarrierBlockMixin implements SimpleWaterloggedBlock {
	@Override
	public boolean canPlaceLiquid(final @Nullable LivingEntity user, final BlockGetter level, final BlockPos pos, final BlockState state, final Fluid type) {
		return !Passthrough.active && type == Fluids.WATER; // (SimpleWaterloggedBlock's own answer otherwise)
	}
}
