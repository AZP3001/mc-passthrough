package dev.rehan.passthrough;

import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.RenderShape;
import net.minecraft.world.level.block.state.BlockBehaviour;
import net.minecraft.world.level.block.state.BlockState;

/**
 * The blocks mining the host's world used (gta_rock, gta_soil, gta_sand, gta_wood, gta_metal), kept registered with
 * nothing to them (unseen, nothing collides, nothing drops): a world played with them would otherwise stop at
 * Fabric's "missing content" screen. Mining the host's world is gone.
 */
public final class LegacyBlocks {
	private LegacyBlocks() {
	}

	static void register() {
		for (String kind : new String[] {"rock", "soil", "sand", "wood", "metal"}) {
			ResourceKey<Block> id = ResourceKey.create(Registries.BLOCK, Identifier.fromNamespaceAndPath("passthrough", "gta_" + kind));
			BlockBehaviour.Properties p = BlockBehaviour.Properties.of().setId(id).noCollision().noOcclusion().noLootTable().air()
				.isViewBlocking((state, level, pos, box) -> false).isSuffocating((state, level, pos) -> false);
			Registry.register(BuiltInRegistries.BLOCK, id, new Unseen(p));
		}
	}

	private static final class Unseen extends Block {
		Unseen(final BlockBehaviour.Properties properties) {
			super(properties);
		}

		@Override
		protected RenderShape getRenderShape(final BlockState state) {
			return RenderShape.INVISIBLE;
		}
	}
}
