package dev.rehan.passthrough;

import it.unimi.dsi.fastutil.longs.Long2ObjectMap;
import it.unimi.dsi.fastutil.longs.Long2ObjectOpenHashMap;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.entity.projectile.FishingHook;
import net.minecraft.world.entity.projectile.Projectile;
import net.minecraft.world.phys.shapes.BooleanOp;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.jspecify.annotations.Nullable;

/**
 * The host's own collision near the player (its floors, walls and ceilings, as boxes the host probes for), for the
 * player alone while Minecraft moves it: they stand in for the host's ground blocks (barriers), which are only whole
 * blocks at rounded heights. Each block cell holds the parts of the boxes inside it (BlockStateBaseMixin hands them out
 * as that cell's collision).
 */
public final class HostCollision {
	private static volatile Long2ObjectMap<VoxelShape> cells = new Long2ObjectOpenHashMap<>();

	private HostCollision() {
	}

	/** Boxes {x0,y0,z0,x1,y1,z1, ...} in Minecraft coordinates, replacing the last set. Any thread. */
	public static void update(final double[] b) {
		Long2ObjectOpenHashMap<List<double[]>> parts = new Long2ObjectOpenHashMap<>();
		for (int i = 0; i + 5 < b.length; i += 6) {
			double x0 = Math.min(b[i], b[i + 3]), x1 = Math.max(b[i], b[i + 3]);
			double y0 = Math.min(b[i + 1], b[i + 4]), y1 = Math.max(b[i + 1], b[i + 4]);
			double z0 = Math.min(b[i + 2], b[i + 5]), z1 = Math.max(b[i + 2], b[i + 5]);
			if (x1 - x0 > 8.0 || y1 - y0 > 8.0 || z1 - z0 > 8.0) {
				continue; // not one of the host's (a box is a cell, a wall piece or a ceiling patch)
			}

			for (int x = (int) Math.floor(x0); x < x1; x++) {
				for (int y = (int) Math.floor(y0); y < y1; y++) {
					for (int z = (int) Math.floor(z0); z < z1; z++) {
						double[] local = {
							Math.max(x0, x) - x, Math.max(y0, y) - y, Math.max(z0, z) - z, Math.min(x1, x + 1) - x, Math.min(y1, y + 1) - y, Math.min(z1, z + 1) - z
						};
						if (local[3] - local[0] > 1.0E-4 && local[4] - local[1] > 1.0E-4 && local[5] - local[2] > 1.0E-4) {
							parts.computeIfAbsent(BlockPos.asLong(x, y, z), k -> new ArrayList<>()).add(local);
						}
					}
				}
			}
		}

		Long2ObjectOpenHashMap<VoxelShape> next = new Long2ObjectOpenHashMap<>(parts.size());
		for (Long2ObjectMap.Entry<List<double[]>> e : parts.long2ObjectEntrySet()) {
			VoxelShape shape = Shapes.empty();
			for (double[] p : e.getValue()) {
				shape = Shapes.joinUnoptimized(shape, Shapes.create(p[0], p[1], p[2], p[3], p[4], p[5]), BooleanOp.OR);
			}

			next.put(e.getLongKey(), shape.optimize());
		}

		cells = next;
	}

	public static void clear() {
		cells = new Long2ObjectOpenHashMap<>();
	}

	/** The host's collision inside block cell `pos` (cell coordinates 0..1), or empty. */
	public static VoxelShape at(final BlockPos pos) {
		VoxelShape shape = cells.get(pos.asLong());
		return shape == null ? Shapes.empty() : shape;
	}

	/** The player Minecraft moves: it collides with the host's boxes instead of the host's ground blocks. */
	public static boolean appliesTo(final @Nullable Entity entity) {
		return Passthrough.walking && entity instanceof Player player && !player.isSpectator() && entity.level().isClientSide();
	}

	/**
	 * Steve's projectiles (not the fishing hook) fly through the host's ground blocks: the host traces them through its
	 * own world, walls and ceilings included, and says where they hit.
	 */
	public static boolean passesBarriers(final @Nullable Entity entity) {
		return entity instanceof Projectile projectile && !(entity instanceof FishingHook) && projectile.getOwner() instanceof Player;
	}
}
