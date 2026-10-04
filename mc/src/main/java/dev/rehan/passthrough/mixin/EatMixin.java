package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.Passthrough;
import net.minecraft.core.component.DataComponents;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.food.FoodProperties;
import net.minecraft.world.item.ItemStack;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** The player eats something: the host's player gets its stamina back (and a little health), by how filling it was. */
@Mixin(LivingEntity.class)
abstract class EatMixin {
	@Shadow
	protected ItemStack useItem;

	@Inject(method = "completeUsingItem", at = @At("HEAD"))
	private void passthrough$ate(final CallbackInfo ci) {
		if (!Passthrough.active || !((Object) this instanceof ServerPlayer player) || !player.isUsingItem()) {
			return;
		}

		FoodProperties food = this.useItem.get(DataComponents.FOOD);
		if (food != null) {
			Passthrough.events.accept("{\"t\":\"ate\",\"n\":" + food.nutrition() + "}");
		}
	}
}
