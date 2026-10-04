package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.Passthrough;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** The host's ground (barriers) can't be broken while a host is attached. */
@Mixin(Item.class)
abstract class ItemMixin {
	@Inject(method = "canDestroyBlock", at = @At("HEAD"), cancellable = true)
	private void passthrough$keepGround(final ItemStack stack, final BlockState state, final Level level, final BlockPos pos,
		final LivingEntity user, final CallbackInfoReturnable<Boolean> cir) {
		if (Passthrough.active && state.is(Blocks.BARRIER)) {
			cir.setReturnValue(false);
		}
	}
}
