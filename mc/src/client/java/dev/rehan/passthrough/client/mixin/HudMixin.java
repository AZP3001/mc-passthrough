package dev.rehan.passthrough.client.mixin;

import dev.rehan.passthrough.client.HostBars;
import net.minecraft.client.gui.Hud;
import net.minecraft.client.multiplayer.MultiPlayerGameMode;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.food.FoodData;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Redirect;

/** The hearts, armour and hunger bars show the host's health, armour and stamina (HostBars), in creative too. */
@Mixin(Hud.class)
abstract class HudMixin {
	@Redirect(method = "extractHotbarAndDecorations", at = @At(value = "INVOKE", target = "Lnet/minecraft/client/multiplayer/MultiPlayerGameMode;canHurtPlayer()Z"))
	private boolean passthrough$barsInCreative(final MultiPlayerGameMode gameMode) {
		return gameMode.canHurtPlayer() || HostBars.shown();
	}

	@Redirect(method = "extractPlayerHealth", at = @At(value = "INVOKE", target = "Lnet/minecraft/world/entity/player/Player;getHealth()F"))
	private float passthrough$hostHealth(final Player player) {
		return HostBars.health(player.getHealth());
	}

	@Redirect(method = "extractArmor", at = @At(value = "INVOKE", target = "Lnet/minecraft/world/entity/player/Player;getArmorValue()I"))
	private static int passthrough$hostArmor(final Player player) {
		return HostBars.armor(player.getArmorValue());
	}

	@Redirect(method = "extractFood", at = @At(value = "INVOKE", target = "Lnet/minecraft/world/food/FoodData;getFoodLevel()I"))
	private int passthrough$hostStamina(final FoodData food) {
		return HostBars.food(food.getFoodLevel());
	}
}
