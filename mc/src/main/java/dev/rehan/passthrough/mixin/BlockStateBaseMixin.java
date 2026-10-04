package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.HostCollision;
import dev.rehan.passthrough.Passthrough;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockBehaviour;
import net.minecraft.world.phys.shapes.CollisionContext;
import net.minecraft.world.phys.shapes.EntityCollisionContext;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * The host's ground blocks (barriers) are rough stand-ins for its ground, for Minecraft's mobs and items. The player
 * Minecraft moves collides with the host's real floors, walls and ceilings instead (HostCollision, in the cells where
 * Minecraft has no block of its own), and Steve's projectiles fly through them (the host traces those).
 */
@Mixin(BlockBehaviour.BlockStateBase.class)
abstract class BlockStateBaseMixin {
	@Shadow
	public abstract Block getBlock();

	@Inject(
		method = "getCollisionShape(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;Lnet/minecraft/world/phys/shapes/CollisionContext;)Lnet/minecraft/world/phys/shapes/VoxelShape;",
		at = @At("RETURN"),
		cancellable = true
	)
	private void passthrough$hostCollision(final BlockGetter level, final BlockPos pos, final CollisionContext context, final CallbackInfoReturnable<VoxelShape> cir) {
		if (!Passthrough.active || !(context instanceof EntityCollisionContext entityContext)) {
			return;
		}

		Entity entity = entityContext.getEntity();
		if (entity == null) {
			return;
		}

		boolean barrier = this.getBlock() == Blocks.BARRIER;
		if (HostCollision.appliesTo(entity)) {
			if (barrier || cir.getReturnValue().isEmpty()) {
				cir.setReturnValue(HostCollision.at(pos));
			}
		} else if (barrier && HostCollision.passesBarriers(entity)) {
			cir.setReturnValue(Shapes.empty());
		}
	}
}
