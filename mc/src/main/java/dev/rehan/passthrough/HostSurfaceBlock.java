package dev.rehan.passthrough;

import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.tags.ItemTags;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockBehaviour;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.StateDefinition;
import net.minecraft.world.level.block.state.properties.BlockStateProperties;
import net.minecraft.world.level.block.state.properties.EnumProperty;
import net.minecraft.world.level.block.state.properties.IntegerProperty;
import net.minecraft.world.phys.shapes.CollisionContext;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;

/**
 * The piece of the host's world being mined (a wall, the street, the ground, a door): only where the crosshair is on it
 * with a pickaxe in hand (HostDig). Unseen (its texture is clear), but it shows Minecraft's cracks as it's mined, makes
 * its material's sounds and particles, drops what that material gives, and takes as long as Minecraft's survival
 * mining does: only with a pickaxe. Shaped to the host's surface: the part of the cell behind it (FACING: the way the
 * surface faces; DEPTH: sixteenths of the cell in front of it, on that side; its face just in front of the surface, so
 * the cracks show on it). Nothing collides with it.
 */
public class HostSurfaceBlock extends Block {
	public static final EnumProperty<Direction> FACING = BlockStateProperties.FACING;
	public static final IntegerProperty DEPTH = IntegerProperty.create("depth", 0, 15);
	/** Minecraft's cracks on it as it's mined (0 none, 1..10 its destroy stages), drawn as its own faces: Minecraft's own
	 * cracks darken what's under them, and under this there's nothing of Minecraft's (the host's wall is the host's). */
	public static final IntegerProperty CRACK = IntegerProperty.create("crack", 0, 10);

	/** 0 rock, 1 soil, 2 sand, 3 wood, 4 metal. */
	public final int kind;

	public HostSurfaceBlock(final BlockBehaviour.Properties properties, final int kind) {
		super(properties);
		this.kind = kind;
		this.registerDefaultState(this.stateDefinition.any().setValue(FACING, Direction.UP).setValue(DEPTH, 0).setValue(CRACK, 0));
	}

	@Override
	protected void createBlockStateDefinition(final StateDefinition.Builder<Block, BlockState> builder) {
		builder.add(FACING, DEPTH, CRACK);
	}

	@Override
	protected VoxelShape getShape(final BlockState state, final BlockGetter level, final BlockPos pos, final CollisionContext context) {
		double d = state.getValue(DEPTH);
		return switch (state.getValue(FACING)) {
			case UP -> Block.box(0, 0, 0, 16, 16 - d, 16);
			case DOWN -> Block.box(0, d, 0, 16, 16, 16);
			case NORTH -> Block.box(0, 0, d, 16, 16, 16);
			case SOUTH -> Block.box(0, 0, 0, 16, 16, 16 - d);
			case WEST -> Block.box(d, 0, 0, 16, 16, 16);
			case EAST -> Block.box(0, 0, 0, 16 - d, 16, 16);
		};
	}

	@Override
	protected VoxelShape getCollisionShape(final BlockState state, final BlockGetter level, final BlockPos pos, final CollisionContext context) {
		return Shapes.empty(); // (the host's own collision is there: HostCollision)
	}

	@Override
	protected float getDestroyProgress(final BlockState state, final Player player, final BlockGetter level, final BlockPos pos) {
		// the host's world gives way to a pickaxe only
		return player.getMainHandItem().is(ItemTags.PICKAXES) ? super.getDestroyProgress(state, player, level, pos) : 0.0F;
	}

	@Override
	protected boolean propagatesSkylightDown(final BlockState state) {
		return true;
	}
}
