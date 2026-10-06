package dev.rehan.passthrough;

import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentLinkedQueue;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.SoundType;
import net.minecraft.world.level.block.state.BlockBehaviour;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.MapColor;
import net.minecraft.world.level.material.PushReaction;

/**
 * Mining the host's world with a pickaxe. Where the crosshair is on the host's wall, street, ground or a door with a
 * pickaxe in hand, an unseen HostSurfaceBlock takes that cell's place, and Minecraft mines it as it mines its own:
 * cracks, time by the pickaxe and its enchantments, sounds, particles, drops, the pickaxe's wear. Broken, the cell is dug
 * out: the host is told ({"t":"dug","c":[x,y,z],"k":kind}; it cuts the cell out of its picture, and takes a door or
 * a thing away), Minecraft's player goes through it (HostCollision has none of the host's collision there), and the host
 * says which cells round it are still in its solid world ({"t":"digfill"}): Minecraft fills those with its own blocks,
 * the hole's sides, which mine on in the same way. /clearall puts everything back. Server thread, but for the queues.
 */
public final class HostDig {
	public static final String[] KINDS = {"rock", "soil", "sand", "wood", "metal"};
	/** The block a cell of each kind is filled with round a hole. */
	private static final Block[] FILL = {Blocks.STONE, Blocks.DIRT, Blocks.SAND, Blocks.OAK_PLANKS, Blocks.STONE};
	public static final HostSurfaceBlock[] SURFACE = new HostSurfaceBlock[KINDS.length];

	private record Was(BlockState state, int kind) {
	}

	// from the client (the crosshair, each frame) and the host (cells to fill)
	private static volatile int[] wanted; // {x, y, z, facing, depth, kind, thing}, or null
	private static final ConcurrentLinkedQueue<int[]> fills = new ConcurrentLinkedQueue<>();
	private static volatile boolean clearWanted;

	// what's dug out and filled (read by HostCollision and WorldBridge on any thread), by BlockPos.asLong
	private static final Map<Long, Was> dug = new ConcurrentHashMap<>();
	private static final Map<Long, Was> filled = new ConcurrentHashMap<>();
	// server thread
	private static BlockPos piece;     // where the HostSurfaceBlock is now
	private static BlockState pieceWas; // what was there (air, or the host's ground: a barrier)
	private static boolean changing;   // (our own block changes: not mining)

	private HostDig() {
	}

	/** The blocks (main entrypoint). */
	static void register() {
		SoundType[] sounds = {SoundType.STONE, SoundType.GRAVEL, SoundType.SAND, SoundType.WOOD, SoundType.METAL};
		float[] hardness = {1.5F, 0.6F, 0.5F, 2.0F, 5.0F};
		MapColor[] colors = {MapColor.STONE, MapColor.DIRT, MapColor.SAND, MapColor.WOOD, MapColor.METAL};
		for (int k = 0; k < KINDS.length; k++) {
			ResourceKey<Block> id = ResourceKey.create(Registries.BLOCK, Identifier.fromNamespaceAndPath("passthrough", "gta_" + KINDS[k]));
			BlockBehaviour.Properties p = BlockBehaviour.Properties.of().setId(id).mapColor(colors[k]).sound(sounds[k]).strength(hardness[k], 6.0F)
				.requiresCorrectToolForDrops().noCollision().noOcclusion().pushReaction(PushReaction.IMMOVEABLE)
				.isViewBlocking((state, level, pos, box) -> false).isSuffocating((state, level, pos) -> false);
			SURFACE[k] = Registry.register(BuiltInRegistries.BLOCK, id, new HostSurfaceBlock(p, k));
		}
	}

	public static boolean isDug(final BlockPos pos) {
		return !dug.isEmpty() && dug.containsKey(pos.asLong());
	}

	/** One of the blocks filling round a hole (not to be copied into the host's world: the host's own solid is there). */
	public static boolean isFill(final BlockPos pos) {
		return !filled.isEmpty() && filled.containsKey(pos.asLong());
	}

	/** Cells the host's ground (barriers) mustn't fill: dug out, filled round a hole, or being mined. */
	public static boolean keepOut(final BlockPos pos) {
		return isDug(pos) || isFill(pos) || pos.equals(piece);
	}

	/**
	 * The crosshair is on the host's world at `cell` with a pickaxe in hand (the surface facing `face`, `depth`
	 * sixteenths of the cell in front of it; `kind` its material; `thing`: one of the host's things, a door, a sign, not
	 * its wall or street), or on nothing of the host's (null). Client thread.
	 */
	public static void aim(final BlockPos cell, final Direction face, final int depth, final int kind, final boolean thing) {
		int[] w = wanted;
		if (cell == null) {
			if (w != null) {
				wanted = null;
			}

			return;
		}

		if (w == null || w[0] != cell.getX() || w[1] != cell.getY() || w[2] != cell.getZ() || w[5] != kind || (w[6] != 0) != thing) {
			wanted = new int[] {cell.getX(), cell.getY(), cell.getZ(), face.get3DDataValue(), Math.clamp(depth, 0, 15), Math.clamp(kind, 0, KINDS.length - 1),
				thing ? 1 : 0};
		}
	}

	/** The host's cells to fill round a hole: {x, y, z, kind, ...}. Any thread. */
	public static void fill(final int[] cells) {
		for (int i = 0; i + 3 < cells.length; i += 4) {
			fills.add(new int[] {cells[i], cells[i + 1], cells[i + 2], cells[i + 3]});
		}
	}

	/** A host (re)connected: it knows of nothing dug, so nothing is. Any thread. */
	public static void reset() {
		clearWanted = true;
	}

	static void tick(final MinecraftServer s) {
		ServerLevel level = s.overworld();
		if (clearWanted) {
			clearWanted = false;
			clearAll(level);
		}

		int[] w = Passthrough.active ? wanted : null;
		BlockPos want = w == null ? null : new BlockPos(w[0], w[1], w[2]);
		if (piece != null && (want == null || !want.equals(piece) || !(level.getBlockState(piece).getBlock() instanceof HostSurfaceBlock b) || b.kind != w[5])) {
			restorePiece(level);
		}

		if (want != null && piece == null && level.isLoaded(want) && !isDug(want) && !isFill(want)) {
			BlockState here = level.getBlockState(want);
			if (here.isAir() || here.is(Blocks.BARRIER)) {
				BlockState state = SURFACE[w[5]].defaultBlockState().setValue(HostSurfaceBlock.FACING, Direction.from3DDataValue(w[3]))
					.setValue(HostSurfaceBlock.DEPTH, w[4]);
				pieceWas = here;
				pieceThing = w[6] != 0;
				piece = want;
				set(level, want, state);
			}
		}

		for (int[] f; (f = fills.poll()) != null;) {
			BlockPos pos = new BlockPos(f[0], f[1], f[2]);
			long k = pos.asLong();
			if (dug.containsKey(k) || filled.containsKey(k) || pos.equals(piece) || !level.isLoaded(pos)) {
				continue;
			}

			BlockState here = level.getBlockState(pos);
			if (here.isAir() || here.is(Blocks.BARRIER)) {
				int kind = Math.clamp(f[3], 0, KINDS.length - 1);
				filled.put(k, new Was(here, kind)); // (before it's there: it's never copied into the host's world)
				set(level, pos, FILL[kind].defaultBlockState());
			}
		}
	}

	/** How far the piece has been mined (ServerLevel.destroyBlockProgress: 0..9, -1 let go): its cracks show it. */
	public static void progress(final ServerLevel level, final BlockPos pos, final int stage) {
		if (piece == null || !pos.equals(piece) || !(level.getBlockState(pos).getBlock() instanceof HostSurfaceBlock)) {
			return;
		}

		BlockState state = level.getBlockState(pos);
		int crack = stage >= 0 && stage <= 9 ? stage + 1 : 0;
		if (state.getValue(HostSurfaceBlock.CRACK) != crack) {
			set(level, pos, state.setValue(HostSurfaceBlock.CRACK, crack));
		}
	}

	/** Every block change on the server (LevelMixin, through WorldBridge): the piece or a filling block gone is dug out. */
	static void onBlockChanged(final BlockPos pos, final BlockState state) {
		if (changing) {
			return;
		}

		long k = pos.asLong();
		if (pos.equals(piece) && !(state.getBlock() instanceof HostSurfaceBlock)) {
			// mined (or blown up)
			int kind = pieceKind;
			dug.put(k, new Was(pieceWas, kind));
			piece = null;
			dugOut(pos, kind, pieceThing);
		} else if (!filled.isEmpty() && filled.containsKey(k)) {
			Was was = filled.get(k);
			if (state.is(FILL[was.kind()])) {
				return;
			}

			filled.remove(k);
			dug.put(k, was);
			dugOut(pos, was.kind(), false);
		}
	}

	private static int pieceKind;
	private static boolean pieceThing; // the piece is on one of the host's things (a door, a sign), not its wall

	/** The host is told a cell is mined out (`thing`: it was one of its things there, which goes, not its wall). */
	private static void dugOut(final BlockPos pos, final int kind, final boolean thing) {
		Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"dug\",\"c\":[%d,%d,%d],\"k\":%d,\"o\":%d}", pos.getX(), pos.getY(), pos.getZ(),
			kind, thing ? 1 : 0));
	}

	private static void set(final ServerLevel level, final BlockPos pos, final BlockState state) {
		if (state.getBlock() instanceof HostSurfaceBlock b) {
			pieceKind = b.kind;
		}

		changing = true;
		try {
			level.setBlock(pos, state, Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
		} finally {
			changing = false;
		}
	}

	private static void restorePiece(final ServerLevel level) {
		if (piece != null && level.getBlockState(piece).getBlock() instanceof HostSurfaceBlock) {
			set(level, piece, pieceWas != null ? pieceWas : Blocks.AIR.defaultBlockState());
		}

		piece = null;
		pieceWas = null;
	}

	/** Everything mined put back (/clearall, the world closing, a new host): the host's ground where it was. */
	static void clearAll(final ServerLevel level) {
		restorePiece(level);
		for (Map.Entry<Long, Was> e : filled.entrySet()) {
			BlockPos pos = BlockPos.of(e.getKey());
			if (level.isLoaded(pos) && level.getBlockState(pos).is(FILL[e.getValue().kind()])) {
				set(level, pos, e.getValue().state());
			}
		}

		filled.clear();
		for (Map.Entry<Long, Was> e : dug.entrySet()) {
			BlockPos pos = BlockPos.of(e.getKey());
			if (level.isLoaded(pos) && level.getBlockState(pos).isAir()) {
				set(level, pos, e.getValue().state());
			}
		}

		dug.clear();
		fills.clear();
	}
}
