package dev.rehan.passthrough.client.mixin;

import com.mojang.blaze3d.vertex.PoseStack;
import dev.rehan.passthrough.Passthrough;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.FirstPersonHandsAndItemsRenderer;
import net.minecraft.client.renderer.SubmitNodeCollector;
import net.minecraft.client.renderer.state.level.FirstPersonHandsAndItemsRenderState;
import net.minecraft.client.renderer.state.level.PlayerRenderState;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.InteractionHand;
import net.minecraft.world.item.ItemStack;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** The off hand isn't drawn in first person (it covered the host's HUD), unless it's being used (a shield, food). */
@Mixin(FirstPersonHandsAndItemsRenderer.class)
abstract class FirstPersonHandsAndItemsRendererMixin {
	@Inject(method = "submitArmWithItem", at = @At("HEAD"), cancellable = true)
	private void passthrough$noOffHand(final PlayerRenderState playerState, final FirstPersonHandsAndItemsRenderState state, final float partialTicks,
		final float xRot, final InteractionHand hand, final float attack, final ItemStack itemStack, final float inverseArmHeight,
		final PoseStack poseStack, final SubmitNodeCollector submitNodeCollector, final int lightCoords, final CallbackInfo ci) {
		LocalPlayer player = Minecraft.getInstance().player;
		if (Passthrough.active && hand == InteractionHand.OFF_HAND && !(player != null && player.isUsingItem() && player.getUsedItemHand() == hand)) {
			ci.cancel();
		}
	}
}
