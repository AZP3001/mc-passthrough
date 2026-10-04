package dev.rehan.passthrough.mixin;

import net.minecraft.world.entity.projectile.Projectile;
import net.minecraft.world.phys.HitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Invoker;

/** Ends a projectile where the host traced it into something of its own, as if it hit a block there. */
@Mixin(Projectile.class)
public interface ProjectileInvoker {
	@Invoker("onHit")
	void passthrough$onHit(HitResult hitResult);
}
