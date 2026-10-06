package dev.rehan.passthrough.mixin;

import net.minecraft.util.Mth;
import net.minecraft.world.entity.ai.attributes.Attribute;
import net.minecraft.world.entity.ai.attributes.RangedAttribute;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** The reach (/range) goes past Minecraft's cap of 64 blocks, up to 512. */
@Mixin(RangedAttribute.class)
abstract class RangedAttributeMixin {
	@Inject(method = "sanitizeValue", at = @At("HEAD"), cancellable = true)
	private void passthrough$longReach(final double value, final CallbackInfoReturnable<Double> cir) {
		String id = ((Attribute) (Object) this).getDescriptionId();
		if ("attribute.name.block_interaction_range".equals(id) || "attribute.name.entity_interaction_range".equals(id)) {
			cir.setReturnValue(Double.isNaN(value) ? 0.0 : Mth.clamp(value, 0.0, 512.0));
		}
	}
}
