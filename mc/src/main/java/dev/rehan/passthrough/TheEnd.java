package dev.rehan.passthrough;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.UUID;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.util.RandomSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntitySpawnReason;
import net.minecraft.world.entity.EntityTypes;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.boss.enderdragon.EndCrystal;
import net.minecraft.world.entity.boss.enderdragon.EnderDragon;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.EndPortalFrameBlock;
import net.minecraft.world.level.block.state.BlockState;

/**
 * The End comes to the host's world: walking into an end portal (built from frames and eyes, or with /endportal) turns
 * the ground around it into end stone with obsidian pillars crowned by end crystals, endermen come, and after a while
 * the ender dragon arrives and circles the portal. The host is told ("end") so it turns its own world to midnight and
 * fog. Walking into the portal again (after stepping out of it) closes the End and puts everything back; the portal
 * itself stays, so it can be opened again. Server thread only.
 */
public final class TheEnd {
	public static final String TAG = "passthrough_end";
	private static final int RADIUS = 24;
	private static final int SPREAD_TICKS = 160;
	private static final int DRAGON_AT = 20 * 25;
	private static final int MAX_ENDERMEN = 10;
	private static final int FLAGS = Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE;

	private static boolean active;
	private static boolean playerInPortal;
	private static int ticks;
	private static double centerX, centerZ;
	private static int portalY;
	/** Every block the End changed, with what was there before (put back on close). */
	private static final Map<BlockPos, BlockState> changed = new LinkedHashMap<>();
	private static final List<int[]> columns = new ArrayList<>(); // {x, z, distance * 1000}
	private static int nextColumn;
	private static UUID dragon;
	private static boolean dragonDown;
	private static final RandomSource random = RandomSource.create();
	/** Endermen in the host's cars they stole: where the host says the car's seat is (x, y, z, yaw), or null: out again. */
	private static final Map<Integer, double[]> rides = new java.util.concurrent.ConcurrentHashMap<>();
	private static final java.util.Set<Integer> riding = new java.util.HashSet<>();
	private static final double[] OUT = new double[0];

	private TheEnd() {
	}

	static void tick(final MinecraftServer s) {
		ServerLevel level = s.overworld();
		ServerPlayer player = s.getPlayerList().getPlayers().isEmpty() ? null : s.getPlayerList().getPlayers().get(0);
		if (player != null && player.level() == level) {
			// end portal blocks are low: the player stands in them, or just on them (a little above: the host's
			// feet are rarely exactly on a whole block)
			BlockPos feet = BlockPos.containing(player.getX(), player.getY() + 0.2, player.getZ());
			BlockPos in = level.getBlockState(feet).is(Blocks.END_PORTAL) ? feet
				: level.getBlockState(feet.below()).is(Blocks.END_PORTAL) ? feet.below() : null;
			// stepping into the portal opens the End, stepping into it again closes it
			boolean entered = in != null && !playerInPortal;
			if ((in != null) != playerInPortal) {
				playerInPortal = in != null;
				Passthrough.events.accept("{\"t\":\"inportal\",\"on\":" + playerInPortal + "}");
			}

			if (entered && !active && Passthrough.active) {
				start(level, in);
			} else if (entered && active) {
				close(level);
			}
		}

		if (active) {
			ticks++;
			spread(level);
			waves(level);
			dragonTick(level);
			thieves(level);
		}

		ridesTick(level);
	}

	/** The host: enderman `id` is in a stolen car at (x, y, z, yaw), or (null) out of it again. Any thread. */
	public static void ride(final int id, final double[] at) {
		rides.put(id, at == null ? OUT : at);
	}

	/** Now and then an enderman near the player goes for one of the host's cars (the host picks it, "enderthief"). */
	private static void thieves(final ServerLevel level) {
		if (ticks % 60 != 0 || !Passthrough.active) {
			return;
		}

		for (Entity e : level.getAllEntities()) {
			if (e.entityTags().contains(TAG) && e instanceof Mob && e.isAlive() && e.getType() == EntityTypes.ENDERMAN
				&& !riding.contains(e.getId()) && random.nextFloat() < 0.2F) {
				Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"enderthief\",\"id\":%d,\"pos\":[%.2f,%.2f,%.2f]}",
					e.getId(), e.getX(), e.getY(), e.getZ()));
			}
		}
	}

	/** Endermen in stolen cars go where the cars are; let out, they're themselves again. */
	private static void ridesTick(final ServerLevel level) {
		if (rides.isEmpty()) {
			return;
		}

		for (Map.Entry<Integer, double[]> r : rides.entrySet()) {
			int id = r.getKey();
			double[] at = rides.remove(id);
			if (at == null) {
				continue;
			}

			if (!(level.getEntity(id) instanceof Mob mob) || !mob.isAlive()) {
				riding.remove(id);
				continue;
			}

			if (at == OUT) {
				riding.remove(id);
				mob.setNoAi(false);
				mob.setNoGravity(false);
				continue;
			}

			riding.add(id);
			mob.setNoAi(true);
			mob.setNoGravity(true);
			mob.teleportTo(at[0], at[1], at[2]);
			mob.setYRot((float) at[3]);
			mob.setYHeadRot((float) at[3]);
			mob.setYBodyRot((float) at[3]);
			mob.setDeltaMovement(net.minecraft.world.phys.Vec3.ZERO);
		}
	}

	private static void start(final ServerLevel level, final BlockPos portal) {
		Nether.closeIfOpen(level); // one realm at a time
		// the portal's middle: the centre of the connected end portal blocks around this one
		int minX = portal.getX(), maxX = minX, minZ = portal.getZ(), maxZ = minZ;
		for (int dx = -3; dx <= 3; dx++) {
			for (int dz = -3; dz <= 3; dz++) {
				if (level.getBlockState(portal.offset(dx, 0, dz)).is(Blocks.END_PORTAL)) {
					minX = Math.min(minX, portal.getX() + dx);
					maxX = Math.max(maxX, portal.getX() + dx);
					minZ = Math.min(minZ, portal.getZ() + dz);
					maxZ = Math.max(maxZ, portal.getZ() + dz);
				}
			}
		}

		centerX = (minX + maxX + 1) / 2.0;
		centerZ = (minZ + maxZ + 1) / 2.0;
		portalY = portal.getY();
		active = true;
		ticks = 0;
		dragon = null;
		dragonDown = false;
		planSpread();
		WorldBridge.command("gamerule mob_griefing false"); // endermen and the dragon leave the builds alone
		Passthrough.LOG.info("the end: portal at {} {} {}", centerX, portalY, centerZ);
		Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"end\",\"on\":true,\"pos\":[%.2f,%d,%.2f]}", centerX, portalY, centerZ));
	}

	/** The ground turns in a ragged circle growing out of the portal. */
	private static void planSpread() {
		columns.clear();
		nextColumn = 0;
		int cx = (int) Math.floor(centerX), cz = (int) Math.floor(centerZ);
		for (int dx = -RADIUS; dx <= RADIUS; dx++) {
			for (int dz = -RADIUS; dz <= RADIUS; dz++) {
				double ragged = Math.sqrt(dx * dx + dz * dz) + 3.0 * random.nextDouble();
				if (ragged <= RADIUS) {
					columns.add(new int[] {cx + dx, cz + dz, (int) (ragged * 1000)});
				}
			}
		}

		columns.sort((a, b) -> Integer.compare(a[2], b[2]));
	}

	private static void spread(final ServerLevel level) {
		if (nextColumn >= columns.size()) {
			return;
		}

		double reach = RADIUS * Math.min(1.0, ticks / (double) SPREAD_TICKS) + 1.5;
		WorldBridge.quietGround(true);
		try {
			while (nextColumn < columns.size() && columns.get(nextColumn)[2] <= reach * 1000) {
				int[] c = columns.get(nextColumn++);
				turn(level, c[0], c[1]);
			}
		} finally {
			WorldBridge.quietGround(false);
		}
	}

	/** One column of the host's ground (its top barrier, near the portal's height) turns to end stone. */
	private static void turn(final ServerLevel level, final int x, final int z) {
		BlockPos.MutableBlockPos p = new BlockPos.MutableBlockPos();
		int top = Integer.MIN_VALUE;
		for (int y = portalY + 5; y >= portalY - 12; y--) {
			p.set(x, y, z);
			if (level.getBlockState(p).is(Blocks.BARRIER) && level.getBlockState(p.above()).isAir()) {
				top = y;
				break;
			}
		}

		if (top == Integer.MIN_VALUE) {
			return; // no host ground here (or something of the player's on it)
		}

		BlockPos ground = new BlockPos(x, top, z);
		set(level, ground, Blocks.END_STONE.defaultBlockState());
		double away = Math.hypot(x + 0.5 - centerX, z + 0.5 - centerZ);
		double r = random.nextDouble();
		if (away > 9.0 && r < 0.006) {
			pillar(level, ground.above());
		} else if (away > 5.0 && r < 0.02) {
			// a little chorus plant
			int h = 1 + random.nextInt(3);
			for (int k = 0; k < h; k++) {
				set(level, ground.above(k + 1), Blocks.CHORUS_PLANT.defaultBlockState());
			}

			set(level, ground.above(h + 1), Blocks.CHORUS_FLOWER.defaultBlockState());
		}
	}

	/** An obsidian pillar (solid: the host's people and cars stop at it) with an end crystal on top. */
	private static void pillar(final ServerLevel level, final BlockPos base) {
		int h = 4 + random.nextInt(5);
		WorldBridge.quietGround(false);
		for (int k = 0; k < h; k++) {
			set(level, base.above(k), Blocks.OBSIDIAN.defaultBlockState());
		}

		set(level, base.above(h), Blocks.BEDROCK.defaultBlockState());
		WorldBridge.quietGround(true);
		EndCrystal crystal = EntityTypes.END_CRYSTAL.create(level, EntitySpawnReason.COMMAND);
		if (crystal != null) {
			crystal.snapTo(base.getX() + 0.5, base.getY() + h + 1, base.getZ() + 0.5, 0.0F, 0.0F);
			crystal.addTag(TAG);
			level.addFreshEntity(crystal);
		}
	}

	private static void set(final ServerLevel level, final BlockPos pos, final BlockState state) {
		BlockPos p = pos.immutable();
		changed.putIfAbsent(p, level.getBlockState(p));
		level.setBlock(p, state, FLAGS);
	}

	/** Endermen keep coming out of the portal, a few at a time. */
	private static void waves(final ServerLevel level) {
		if (ticks == 40 || (ticks > 40 && ticks % 300 == 0)) {
			int endermen = 0;
			for (Entity e : level.getAllEntities()) {
				if (e.entityTags().contains(TAG) && e instanceof Mob && e.isAlive()) {
					endermen++;
				}
			}

			for (int i = 0; i < 3 && endermen < MAX_ENDERMEN; i++) {
				double a = random.nextDouble() * Math.PI * 2.0, d = 4.0 + random.nextDouble() * 4.0;
				BlockPos at = BlockPos.containing(centerX + Math.cos(a) * d, portalY, centerZ + Math.sin(a) * d);
				Entity e = EntityTypes.ENDERMAN.spawn(level, at, EntitySpawnReason.COMMAND);
				if (e instanceof Mob mob) {
					mob.setPersistenceRequired();
					mob.addTag(TAG);
					endermen++;
				}
			}
		}
	}

	/** The dragon arrives after a while and circles the portal; once it's down, the way home is the portal. */
	private static void dragonTick(final ServerLevel level) {
		if (ticks == DRAGON_AT && dragon == null) {
			EnderDragon d = EntityTypes.ENDER_DRAGON.create(level, EntitySpawnReason.COMMAND);
			if (d != null) {
				d.snapTo(centerX, portalY + 40, centerZ, 0.0F, 0.0F);
				d.setFightOrigin(BlockPos.containing(centerX, portalY, centerZ));
				d.addTag(TAG);
				d.setPersistenceRequired();
				level.addFreshEntity(d);
				dragon = d.getUUID();
				Passthrough.events.accept("{\"t\":\"note\",\"text\":\"~p~The ender dragon is coming!\"}");
			}
		}

		if (dragon != null && !dragonDown && ticks % 20 == 0) {
			Entity d = level.getEntity(dragon);
			if (d == null || !d.isAlive()) {
				dragonDown = true;
				Passthrough.events.accept("{\"t\":\"note\",\"text\":\"~g~The dragon is dead!~s~ Walk into the portal to go home.\"}");
			}
		}
	}

	/** Close the End: everything it changed goes back, its mobs and crystals go, the host's night lifts. */
	private static void close(final ServerLevel level) {
		List<BlockPos> order = new ArrayList<>(changed.keySet());
		order.sort((a, b) -> Integer.compare(b.getY(), a.getY())); // top-down
		for (BlockPos p : order) {
			level.setBlock(p, changed.get(p), FLAGS);
		}

		boolean wasOpen = active;
		changed.clear();
		columns.clear();
		active = false;
		dragon = null;
		List<Entity> all = new ArrayList<>();
		level.getAllEntities().forEach(all::add);
		all.stream().filter(e -> e.entityTags().contains(TAG)).forEach(Entity::discard);
		if (wasOpen) {
			WorldBridge.command("gamerule mob_griefing true");
		}

		Passthrough.events.accept("{\"t\":\"end\",\"on\":false}");
		Passthrough.LOG.info("the end: closed");
	}

	/** Server thread: the End closes (the Nether is opening). */
	static void closeIfOpen(final ServerLevel level) {
		if (active) {
			close(level);
		}
	}

	/** A block of the host's ground the End turned (end stone: not something to collide with). */
	static boolean isGround(final BlockPos pos) {
		BlockState before = changed.get(pos);
		return before != null && before.is(Blocks.BARRIER);
	}

	/** The host (re)connected: tell it again that the End is open. */
	public static void resync() {
		MinecraftServer s = WorldBridge.server();
		if (s != null) {
			s.execute(() -> {
				if (active) {
					Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"end\",\"on\":true,\"pos\":[%.2f,%d,%.2f]}", centerX, portalY, centerZ));
				}
			});
		}
	}

	/** Build a ready end portal (frames with eyes round a 3x3 portal) on the ground about 5 blocks ahead of the player. */
	public static void buildPortal(final ServerPlayer player) {
		ServerLevel level = (ServerLevel) player.level();
		double yaw = Math.toRadians(player.getYRot());
		int bx = (int) Math.floor(player.getX() - Math.sin(yaw) * 5.0), bz = (int) Math.floor(player.getZ() + Math.cos(yaw) * 5.0);
		int ground = Integer.MIN_VALUE;
		BlockPos.MutableBlockPos p = new BlockPos.MutableBlockPos();
		for (int y = (int) Math.floor(player.getY()) + 3; y >= (int) Math.floor(player.getY()) - 8; y--) {
			p.set(bx, y, bz);
			if (!level.getBlockState(p).isAir() && level.getBlockState(p.above()).isAir()) {
				ground = y;
				break;
			}
		}

		if (ground == Integer.MIN_VALUE) {
			ground = (int) Math.floor(player.getY()) - 1;
		}

		int y = ground + 1;
		for (int dx = -2; dx <= 2; dx++) {
			for (int dz = -2; dz <= 2; dz++) {
				BlockPos at = new BlockPos(bx + dx, y, bz + dz);
				boolean ring = Math.max(Math.abs(dx), Math.abs(dz)) == 2;
				if (ring && Math.abs(dx) == 2 && Math.abs(dz) == 2) {
					continue; // no corners
				}

				if (ring) {
					// each frame faces the middle, with its eye in
					Direction facing = dx == -2 ? Direction.EAST : dx == 2 ? Direction.WEST : dz == -2 ? Direction.SOUTH : Direction.NORTH;
					level.setBlock(at, Blocks.END_PORTAL_FRAME.defaultBlockState().setValue(EndPortalFrameBlock.FACING, facing)
						.setValue(EndPortalFrameBlock.HAS_EYE, true), Block.UPDATE_ALL);
				} else {
					level.setBlock(at, Blocks.END_PORTAL.defaultBlockState(), Block.UPDATE_ALL);
				}
			}
		}
	}

	static void detach(final MinecraftServer s) {
		if (active || !changed.isEmpty()) {
			close(s.overworld());
		}

		active = false;
		playerInPortal = false;
		changed.clear();
		columns.clear();
	}
}
