package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.HostBridge;
import net.minecraft.commands.CommandSourceStack;
import net.minecraft.commands.Commands;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Commands that reach the host's world too (HostBridge.command): /kill @e, /time, /weather. */
@Mixin(Commands.class)
abstract class CommandsMixin {
	@Inject(method = "performPrefixedCommand", at = @At("HEAD"))
	private void passthrough$before(final CommandSourceStack source, final String command, final CallbackInfo ci) {
		HostBridge.command(source.getServer(), command, false);
	}

	@Inject(method = "performPrefixedCommand", at = @At("TAIL"))
	private void passthrough$after(final CommandSourceStack source, final String command, final CallbackInfo ci) {
		HostBridge.command(source.getServer(), command, true);
	}
}
