package dev.rehan.passthrough;

import it.unimi.dsi.fastutil.longs.Long2LongMap;
import it.unimi.dsi.fastutil.longs.Long2LongOpenHashMap;
import net.minecraft.core.BlockPos;
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
		Long2LongOpenHashMap next = new Long2LongOpenHashMap(columns);
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
