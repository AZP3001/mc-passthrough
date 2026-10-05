package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.HostWater;
import dev.rehan.passthrough.Passthrough;
import net.minecraft.world.entity.EntityFluidInteraction;
import net.minecraft.world.level.Level;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Before looking for water block by block, Minecraft asks the chunk sections whether they hold any fluid at all; the
 * host's water (HostWater) is in none of them, so nobody ever swam in it, and a trident's riptide never launched there.
 * Where the host has water in the box (and its chunks are loaded), the look goes ahead.
 */
@Mixin(EntityFluidInteraction.class)
abstract class EntityFluidMixin {
	@Inject(method = "hasFluidAndLoaded(Lnet/minecraft/world/level/Level;IIIIII)Z", at = @At("RETURN"), cancellable = true)
	private static void passthrough$hostWater(final Level level, final int x0, final int y0, final int z0, final int x1, final int y1, final int z1,
		final CallbackInfoReturnable<Boolean> cir) {
		if (!cir.getReturnValueZ() && Passthrough.active && level.dimension() == Level.OVERWORLD && HostWater.any(level, x0, y0, z0, x1, y1, z1)) {
			cir.setReturnValue(true);
		}
	}
}
