package dev.rehan.passthrough.client.mixin;

import com.mojang.blaze3d.vertex.PoseStack;
import net.minecraft.client.renderer.SubmitNodeCollector;
import net.minecraft.client.renderer.entity.ArrowRenderer;
import net.minecraft.client.renderer.entity.state.ArrowRenderState;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Arrows drawn half their size (their hitbox stays). */
@Mixin(ArrowRenderer.class)
abstract class ArrowRendererMixin {
	@Inject(method = "submit(Lnet/minecraft/client/renderer/entity/state/ArrowRenderState;Lcom/mojang/blaze3d/vertex/PoseStack;Lnet/minecraft/client/renderer/SubmitNodeCollector;Lnet/minecraft/client/renderer/state/level/CameraRenderState;)V",
		at = @At(value = "INVOKE", target = "Lcom/mojang/blaze3d/vertex/PoseStack;pushPose()V", shift = At.Shift.AFTER))
	private void passthrough$half(final ArrowRenderState state, final PoseStack poseStack, final SubmitNodeCollector collector, final CameraRenderState camera, final CallbackInfo ci) {
		poseStack.scale(0.5F, 0.5F, 0.5F);
	}
}
