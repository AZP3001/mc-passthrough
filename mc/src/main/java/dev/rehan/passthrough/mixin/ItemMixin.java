package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.Passthrough;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * The host's ground (barriers) can't be broken while a host is attached. And what an item aims at by itself (a bucket, a
 * boat, a bottle: Item.getPlayerPOVHitResult) includes the host's own surfaces under the crosshair, its walls and streets,
 * as the face of the cell behind them: water poured on the host's street or wall lands in front of it.
 */
@Mixin(Item.class)
abstract class ItemMixin {
	@Inject(method = "canDestroyBlock", at = @At("HEAD"), cancellable = true)
	private void passthrough$keepGround(final ItemStack stack, final BlockState state, final Level level, final BlockPos pos,
		final LivingEntity user, final CallbackInfoReturnable<Boolean> cir) {
		if (Passthrough.active && state.is(Blocks.BARRIER)) {
			cir.setReturnValue(false);
		}
	}

	@Inject(method = "getPlayerPOVHitResult", at = @At("RETURN"), cancellable = true)
	private static void passthrough$hostSurface(final Level level, final Player player, final ClipContext.Fluid fluid,
		final CallbackInfoReturnable<BlockHitResult> cir) {
		double[] h = Passthrough.hostHit;
		if (!Passthrough.active || h == null || h.length < 6) {
			return;
		}

		Vec3 eye = player.getEyePosition();
		Vec3 at = new Vec3(h[0], h[1], h[2]);
		double reach = player.blockInteractionRange() + 1.0;
		BlockHitResult own = cir.getReturnValue();
		double ownDist = own.getType() == HitResult.Type.MISS ? Double.MAX_VALUE : own.getLocation().distanceTo(eye);
		double hostDist = at.distanceTo(eye);
		if (hostDist > reach || hostDist + 0.02 >= ownDist) {
			return;
		}

		Direction face = Direction.getApproximateNearest(h[3], h[4], h[5]);
		BlockPos behind = BlockPos.containing(at.x - face.getStepX() * 0.5, at.y - face.getStepY() * 0.5, at.z - face.getStepZ() * 0.5);
		cir.setReturnValue(new BlockHitResult(at, face, behind, false));
	}
}
