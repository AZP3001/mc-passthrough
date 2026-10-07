package dev.rehan.passthrough.mixin;

import net.minecraft.sounds.SoundEvent;
import net.minecraft.world.entity.projectile.arrow.AbstractArrow;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;
import org.spongepowered.asm.mixin.gen.Invoker;

/** An arrow stuck where the host says it hit: in the ground, as far as both sides are concerned. */
@Mixin(AbstractArrow.class)
public interface AbstractArrowAccessor {
	@Invoker("setInGround")
	void passthrough$setInGround(boolean inGround);

	@Invoker("isInGround")
	boolean passthrough$isInGround();

	@Invoker("getHitGroundSoundEvent")
	SoundEvent passthrough$hitSound();

	@Accessor("baseDamage")
	double passthrough$baseDamage();

	@Accessor("lastState")
	void passthrough$setLastState(BlockState state);
}
