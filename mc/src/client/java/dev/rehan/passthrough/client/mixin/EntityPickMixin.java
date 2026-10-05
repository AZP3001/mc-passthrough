package dev.rehan.passthrough.client.mixin;

import dev.rehan.passthrough.HostDig;
import dev.rehan.passthrough.HostSurfaceBlock;
import dev.rehan.passthrough.client.HostState;
import dev.rehan.passthrough.client.PlayerSync;
import net.minecraft.client.Camera;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.tags.ItemTags;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.Redirect;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * In third person the host camera sits behind and beside the player, so aim along the camera's ray (what is under
 * the crosshair) instead of from the player's eyes; the ray starts level with the player and reaches `range` past.
 * Where the host's own walls, floors or ceilings are nearer than any of Minecraft's blocks, the crosshair is on them:
 * the cell in front of the surface, clicked from outside, so a block placed there sits against the host's wall.
 * While Minecraft moves the player, W goes where the host's camera looks, not where Steve's head turns to aim.
 */
@Mixin(Entity.class)
abstract class EntityPickMixin {
	@Inject(method = "pick(DFZ)Lnet/minecraft/world/phys/HitResult;", at = @At("HEAD"), cancellable = true)
	private void passthrough$cameraRay(final double range, final float a, final boolean withLiquids, final CallbackInfoReturnable<HitResult> cir) {
		HostState.Pose p = HostState.frame();
		if (p == null || !((Object) this instanceof LocalPlayer self)) {
			return;
		}

		Vec3 start, end;
		if (p.firstPerson()) {
			start = self.getEyePosition(a);
			end = start.add(self.getViewVector(a).scale(range));
		} else {
			Camera camera = Minecraft.getInstance().gameRenderer.mainCamera();
			Vec3 from = camera.position();
			Vec3 dir = new Vec3(camera.forwardVector()).normalize();
			double along = Math.max(0.0, self.getEyePosition(a).subtract(from).dot(dir));
			start = from.add(dir.scale(Math.max(0.0, along - 0.4)));
			end = from.add(dir.scale(along + range));
		}

		ClipContext.Fluid fluid = withLiquids ? ClipContext.Fluid.ANY : ClipContext.Fluid.NONE;
		HitResult own = self.level().clip(new ClipContext(start, end, ClipContext.Block.OUTLINE, fluid, self));
		double[] h = p.hostHit();
		if (h != null && h.length >= 6) {
			Vec3 at = new Vec3(h[0], h[1], h[2]);
			double hostDist = at.distanceTo(start);
			double ownDist = own.getType() == HitResult.Type.MISS ? Double.MAX_VALUE : own.getLocation().distanceTo(start);
			if (hostDist + 0.02 < ownDist && hostDist <= end.distanceTo(start)) {
				Direction face = Direction.getApproximateNearest(h[3], h[4], h[5]);
				// a pickaxe in hand, on the host's own wall, street, ground or a thing of it (a door; not a car): the cell
				// just behind the surface is mined (HostDig puts the piece there), else the one in front of it gets blocks
				if (h.length >= 8 && (h[7] == 0.0 || h[7] == 3.0) && self.getMainHandItem().is(ItemTags.PICKAXES)) {
					BlockPos cell = BlockPos.containing(at.x - face.getStepX() * 0.02, at.y - face.getStepY() * 0.02, at.z - face.getStepZ() * 0.02);
					// (its face at or just in front of the host's: the cracks on it aren't hidden behind the host's wall)
					HostDig.aim(cell, face, (int) Math.floor(inFront(cell, face, at) * 16.0), (int) h[6]);
					cir.setReturnValue(new BlockHitResult(at, face, cell, false));
					return;
				}

				HostDig.aim(null, face, 0, 0);
				BlockPos cell = BlockPos.containing(at.x + face.getStepX() * 0.5, at.y + face.getStepY() * 0.5, at.z + face.getStepZ() * 0.5);
				cir.setReturnValue(new BlockHitResult(at, face, cell, false));
				return;
			}
		}

		// (on the piece being mined: it stays; on anything else, it goes)
		if (!(own instanceof BlockHitResult b && own.getType() == HitResult.Type.BLOCK && self.level().getBlockState(b.getBlockPos()).getBlock() instanceof HostSurfaceBlock)) {
			HostDig.aim(null, Direction.UP, 0, 0);
		}

		cir.setReturnValue(own);
	}

	/** How much of `cell` is in front of the host's surface at `at` (facing `face`): 0..15/16 of it. */
	private static double inFront(final BlockPos cell, final Direction face, final Vec3 at) {
		double d = switch (face) {
			case EAST -> cell.getX() + 1 - at.x;
			case WEST -> at.x - cell.getX();
			case UP -> cell.getY() + 1 - at.y;
			case DOWN -> at.y - cell.getY();
			case SOUTH -> cell.getZ() + 1 - at.z;
			case NORTH -> at.z - cell.getZ();
		};
		return Math.clamp(d, 0.0, 15.0 / 16.0);
	}

	@Redirect(method = "moveRelative", at = @At(value = "INVOKE", target = "Lnet/minecraft/world/entity/Entity;getYRot()F"))
	private float passthrough$moveAlongCamera(final Entity self) {
		return self instanceof LocalPlayer ? PlayerSync.moveYaw(self.getYRot()) : self.getYRot();
	}
}
