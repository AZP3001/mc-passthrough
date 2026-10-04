package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.HostCollision;
import dev.rehan.passthrough.Passthrough;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.projectile.arrow.AbstractArrow;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Redirect;

/** An arrow of Steve's passing through one of the host's ground blocks doesn't count as stuck in it (BlockStateBaseMixin). */
@Mixin(AbstractArrow.class)
abstract class AbstractArrowMixin {
	@Redirect(
		method = "tick",
		at = @At(
			value = "INVOKE",
			target = "Lnet/minecraft/world/level/block/state/BlockState;getCollisionShape(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/phys/shapes/VoxelShape;"
		)
	)
	private VoxelShape passthrough$throughHostGround(final BlockState state, final BlockGetter level, final BlockPos pos) {
		if (Passthrough.active && state.is(Blocks.BARRIER) && HostCollision.passesBarriers((Entity) (Object) this)) {
			return Shapes.empty();
		}

		return state.getCollisionShape(level, pos);
	}
}
