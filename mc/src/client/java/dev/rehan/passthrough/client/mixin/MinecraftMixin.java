package dev.rehan.passthrough.client.mixin;

import dev.rehan.passthrough.client.HostState;
import net.minecraft.client.Minecraft;
import net.minecraft.client.MouseHandler;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Redirect;

/**
 * Holding attack mines on (survival's breaking) only while Minecraft's window has the mouse; with a host the host's
 * window has it (its clicks come over the link), so mining stopped after the first click: nothing was ever mined in
 * survival, of Minecraft's blocks or the host's world.
 */
@Mixin(Minecraft.class)
abstract class MinecraftMixin {
	@Redirect(method = "handleKeybinds", at = @At(value = "INVOKE", target = "Lnet/minecraft/client/MouseHandler;isMouseGrabbed()Z"))
	private boolean passthrough$hostHoldsTheMouse(final MouseHandler mouse) {
		return mouse.isMouseGrabbed() || HostState.frame() != null;
	}
}
