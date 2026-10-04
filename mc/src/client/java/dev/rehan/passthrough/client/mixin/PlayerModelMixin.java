package dev.rehan.passthrough.client.mixin;

import dev.rehan.passthrough.client.SteveRig;
import net.minecraft.client.model.HumanoidModel;
import net.minecraft.client.model.player.PlayerModel;
import net.minecraft.client.renderer.entity.state.AvatarRenderState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Steve's head, arms and legs as the host's character holds them (SteveRig), over Minecraft's own animation. */
@Mixin(PlayerModel.class)
abstract class PlayerModelMixin {
	@SuppressWarnings("unchecked")
	@Inject(method = "setupAnim(Lnet/minecraft/client/renderer/entity/state/AvatarRenderState;)V", at = @At("TAIL"))
	private void passthrough$rig(final AvatarRenderState state, final CallbackInfo ci) {
		// first person: the camera is in his head
		((HumanoidModel<AvatarRenderState>) (Object) this).head.visible = !SteveRig.firstPerson(state);
		SteveRig.Pose rig = SteveRig.of(state);
		if (rig != null) {
			SteveRig.apply((HumanoidModel<AvatarRenderState>) (Object) this, state, rig);
		}
	}
}
