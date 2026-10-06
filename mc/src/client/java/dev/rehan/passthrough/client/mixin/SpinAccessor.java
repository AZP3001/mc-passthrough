package dev.rehan.passthrough.client.mixin;

import net.minecraft.world.entity.LivingEntity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

/** A riptide's spin as the client starts it (its flag only comes from the server a tick later). */
@Mixin(LivingEntity.class)
public interface SpinAccessor {
	@Accessor("autoSpinAttackTicks")
	int passthrough$getSpinTicks();
}
