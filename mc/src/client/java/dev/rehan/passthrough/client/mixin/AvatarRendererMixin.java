package dev.rehan.passthrough.client.mixin;

import com.mojang.blaze3d.vertex.PoseStack;
import com.mojang.math.Axis;
import dev.rehan.passthrough.client.HostState;
import dev.rehan.passthrough.client.SteveRig;
import net.minecraft.client.Minecraft;
import net.minecraft.client.model.HumanoidModel;
import net.minecraft.client.renderer.entity.player.AvatarRenderer;
import net.minecraft.client.renderer.entity.state.AvatarRenderState;
import net.minecraft.world.entity.Avatar;
import net.minecraft.world.item.ItemStack;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * While the host moves its character, Steve takes its pose (SteveRig): turned and leaned as its body, with its head, arms
 * and legs. Holding one of the host's guns (drawn by the host): Steve aims with both arms forward, and holds nothing of his
 * own. Without the host's pose: in a vehicle he sits, dead he lies on his side, red, as Minecraft's dead do.
 */
@Mixin(AvatarRenderer.class)
abstract class AvatarRendererMixin {
	@Inject(method = "extractRenderState(Lnet/minecraft/world/entity/Avatar;Lnet/minecraft/client/renderer/entity/state/AvatarRenderState;F)V", at = @At("TAIL"))
	private void passthrough$hostPose(final Avatar entity, final AvatarRenderState state, final float partialTick, final CallbackInfo ci) {
		HostState.Pose p = HostState.frame();
		Minecraft minecraft = Minecraft.getInstance();
		if (p == null || entity != minecraft.player) {
			return;
		}

		// drawn as tall as the host's character (Minecraft's Steve is 1.875 m). (First person: he isn't drawn at all; the
		// inventory's little Steve is this same player, and must keep his head)
		SteveRig.setLocal(state, p.height() / 1.875F, false);

		// (not while a screen that draws Steve itself is open, the inventory: its little Steve would take the pose too)
		SteveRig.Pose rig = p.rig() != null && p.rig().length >= 24 && !p.walk() && !p.drive()
			&& (minecraft.gui.screen() == null || minecraft.gui.screen() instanceof net.minecraft.client.gui.screens.ChatScreen)
			? SteveRig.solve(p.rig()) : null;
		if (rig != null) {
			state.bodyRot = rig.bodyRot();
			state.isPassenger = false;
			state.isCrouching = false;
			state.deathTime = 0.0F; // (the character's own fall shows how it lies)
			state.hasRedOverlay = p.dead();
		} else {
			if (p.vehicle()) {
				state.isPassenger = true;
			}

			if (p.sneak()) {
				state.isCrouching = true;
			}

			if (p.dead()) {
				state.deathTime = 21.0F;
				state.hasRedOverlay = true;
			}
		}

		SteveRig.set(state, rig);
		if (!p.gun()) {
			return;
		}

		state.rightArmPose = HumanoidModel.ArmPose.CROSSBOW_HOLD;
		state.leftArmPose = HumanoidModel.ArmPose.EMPTY;
		state.rightHandItemState.clear();
		state.leftHandItemState.clear();
		state.rightHandItemStack = ItemStack.EMPTY;
		state.leftHandItemStack = ItemStack.EMPTY;
	}

	/** The body's lean (sitting back, bent over, lying), after its yaw: Ry(180 - bodyRot) Rz(b) Rx(a). */
	@Inject(method = "setupRotations(Lnet/minecraft/client/renderer/entity/state/AvatarRenderState;Lcom/mojang/blaze3d/vertex/PoseStack;FF)V", at = @At("TAIL"))
	private void passthrough$lean(final AvatarRenderState state, final PoseStack poseStack, final float bodyRot, final float entityScale, final CallbackInfo ci) {
		SteveRig.Pose rig = SteveRig.of(state);
		if (rig != null) {
			poseStack.rotate(Axis.ZP, rig.leanB());
			poseStack.rotate(Axis.XP, rig.leanA());
		}

	}

	/** The local player as tall as the host's character (after Minecraft's own 0.9375: about his feet). */
	@Inject(method = "scale(Lnet/minecraft/client/renderer/entity/state/AvatarRenderState;Lcom/mojang/blaze3d/vertex/PoseStack;)V", at = @At("TAIL"))
	private void passthrough$height(final AvatarRenderState state, final PoseStack poseStack, final CallbackInfo ci) {
		float k = SteveRig.scaleOf(state);
		if (k != 1.0F) {
			poseStack.scale(k, k, k);
		}
	}
}
