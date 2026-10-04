package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.HostBridge;
import com.mojang.brigadier.ParseResults;
import net.minecraft.commands.CommandSourceStack;
import net.minecraft.commands.Commands;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Commands that reach the host's world too (HostBridge.command): /kill @e, /time, /weather. (performCommand: the chat's
 * commands go straight there, not through performPrefixedCommand, which only the console's and functions' use.)
 */
@Mixin(Commands.class)
abstract class CommandsMixin {
	@Inject(method = "performCommand", at = @At("HEAD"))
	private void passthrough$before(final ParseResults<CommandSourceStack> parsed, final String command, final CallbackInfo ci) {
		HostBridge.command(parsed.getContext().getSource().getServer(), command, false);
	}

	@Inject(method = "performCommand", at = @At("TAIL"))
	private void passthrough$after(final ParseResults<CommandSourceStack> parsed, final String command, final CallbackInfo ci) {
		HostBridge.command(parsed.getContext().getSource().getServer(), command, true);
	}
}
