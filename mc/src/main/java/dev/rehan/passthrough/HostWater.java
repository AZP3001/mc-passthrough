package dev.rehan.passthrough;

import it.unimi.dsi.fastutil.longs.Long2LongMap;
import it.unimi.dsi.fastutil.longs.Long2LongOpenHashMap;
import net.minecraft.core.BlockPos;
import net.minecraft.core.SectionPos;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import net.minecraft.world.level.material.FluidState;
import net.minecraft.world.level.material.Fluids;

/**
 * The host's own water (the sea, lakes, rivers, pools) as Minecraft water nobody sees: the host draws its water, and
 * Minecraft's fluid queries (Level.getFluidState, see LevelMixin) find a water source in every empty cell under the host's
 * water surface. So Minecraft swims, floats and drowns in it, its mobs swim, boats float, riptide tridents launch, an
 * elytra flight ends in it, fire goes out, as in Minecraft's own water. Columns arrive with the host's ground ("gwater").
 */
public final class HostWater {
	/** Column (x, z) -> its water: bottom cell y in the high half, the surface's height in 1/256 blocks in the low half. */
	private static volatile Long2LongMap columns = new Long2LongOpenHashMap();
	private static final FluidState WATER = Fluids.WATER.getSource(false);

	private HostWater() {
	}

	private static long key(final int x, final int z) {
		return ((long) x << 32) ^ (z & 0xFFFFFFFFL);
	}

	/** {x, z, bottom y, surface y * 256, ...}: the host's water over these columns (surface <= bottom: none). Any thread. */
	public static void update(final int[] c) {
		if (c.length < 4) {
			return;
		}

		// (columns far from these new ones dropped as it's copied: the host sends them again on the way back. Kept
		// them all, it grew for ever and every update copied all of it)
		Long2LongOpenHashMap next = new Long2LongOpenHashMap();
		int cx = c[0], cz = c[1];
		for (Long2LongMap.Entry e : columns.long2LongEntrySet()) {
			long k = e.getLongKey();
			int x = (int) (k >> 32), z = (int) k;
			if (Math.abs(x - cx) <= 256 && Math.abs(z - cz) <= 256) {
				next.put(k, e.getLongValue());
			}
		}

		for (int i = 0; i + 3 < c.length; i += 4) {
			long k = key(c[i], c[i + 1]);
			if (c[i + 3] <= c[i + 2] * 256) {
				next.remove(k);
			} else {
				next.put(k, ((long) c[i + 2] << 32) | (c[i + 3] & 0xFFFFFFFFL));
			}
		}

		columns = next;
	}

	public static void clear() {
		columns = new Long2LongOpenHashMap();
	}

	/**
	 * Whether any of the host's water is in the cells x0..x1, y0..y1, z0..z1, and their chunks are loaded (Minecraft's own
	 * check for fluid in a box only knows its chunks' own fluid).
	 */
	public static boolean any(final Level level, final int x0, final int y0, final int z0, final int x1, final int y1, final int z1) {
		Long2LongMap map = columns;
		if (map.isEmpty()) {
			return false;
		}

		boolean found = false;
		for (int x = x0; x <= x1 && !found; x++) {
			for (int z = z0; z <= z1 && !found; z++) {
				long v = map.getOrDefault(key(x, z), Long.MIN_VALUE);
				found = v != Long.MIN_VALUE && (int) (v >> 32) <= y1 && (int) v >= y0 * 256;
			}
		}

		if (!found) {
			return false;
		}

		for (int cx = SectionPos.blockToSectionCoord(x0); cx <= SectionPos.blockToSectionCoord(x1); cx++) {
			for (int cz = SectionPos.blockToSectionCoord(z0); cz <= SectionPos.blockToSectionCoord(z1); cz++) {
				if (level.getChunk(cx, cz, ChunkStatus.FULL, false) == null) {
					return false;
				}
			}
		}

		return true;
	}

	/** The host's water in this cell (a cell is water when the surface is over its middle), or null. */
	public static FluidState at(final BlockPos pos) {
		Long2LongMap map = columns;
		if (map.isEmpty()) {
			return null;
		}

		long v = map.getOrDefault(key(pos.getX(), pos.getZ()), Long.MIN_VALUE);
		if (v == Long.MIN_VALUE) {
			return null;
		}

		int bottom = (int) (v >> 32);
		int surface256 = (int) v;
		return pos.getY() >= bottom && pos.getY() * 256 + 128 <= surface256 ? WATER : null;
	}
}
