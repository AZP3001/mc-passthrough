package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.Passthrough;
import net.minecraft.world.item.EnderpearlItem;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.ModifyArg;

/** Ender pearls fly about two and a half times as far: thrown 1.6 times as fast (the range grows with its square). */
@Mixin(EnderpearlItem.class)
abstract class EnderpearlItemMixin {
	@ModifyArg(
		method = "use",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/entity/projectile/Projectile;spawnProjectileFromRotation(Lnet/minecraft/world/entity/projectile/Projectile$ProjectileFactory;Lnet/minecraft/server/level/ServerLevel;Lnet/minecraft/world/item/ItemStack;Lnet/minecraft/world/entity/LivingEntity;FFF)Lnet/minecraft/world/entity/projectile/Projectile;"),
		index = 5
	)
	private float passthrough$throwFurther(final float power) {
		return Passthrough.active ? power * 1.6F : power;
	}
}
