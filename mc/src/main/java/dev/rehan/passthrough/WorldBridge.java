package dev.rehan.passthrough;

import dev.rehan.passthrough.mixin.ProjectileInvoker;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.Holder;
import net.minecraft.core.component.DataComponents;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.tags.FluidTags;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EquipmentSlot;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.entity.projectile.FireworkRocketEntity;
import net.minecraft.world.entity.projectile.FishingHook;
import net.minecraft.world.entity.projectile.Projectile;
import net.minecraft.world.entity.projectile.arrow.AbstractArrow;
import net.minecraft.world.entity.projectile.arrow.ThrownTrident;
import net.minecraft.world.entity.projectile.hurtingprojectile.windcharge.WindCharge;
import net.minecraft.world.entity.projectile.throwableitemprojectile.AbstractThrownPotion;
import net.minecraft.world.entity.projectile.throwableitemprojectile.Snowball;
import net.minecraft.world.entity.projectile.throwableitemprojectile.ThrownEgg;
import net.minecraft.world.entity.projectile.throwableitemprojectile.ThrownEnderpearl;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.item.alchemy.PotionContents;
import net.minecraft.world.item.enchantment.Enchantments;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.chunk.LevelChunkSection;
import net.minecraft.world.level.material.FluidState;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.Vec3;

/** Server-side half: the host's collision as invisible barrier blocks, commands, and events back to the host. */
public final class WorldBridge {
	private static volatile MinecraftServer server;
	/** Barriers we placed (so a reset only removes ours, never the player's builds). */
	private static final Set<BlockPos> barriers = ConcurrentHashMap.newKeySet();
	/** Block changes this tick (server thread only): true = now solid, false = gone. Sent at the end of the tick. */
	private static final Map<BlockPos, Boolean> changes = new LinkedHashMap<>();
	/** While placing or removing the host's own ground: those changes aren't news to the host (server thread only). */
	private static boolean placingGround;
	/** Water the host knows of, and water changed this tick (server thread only): it puts out the host's fires. */
	private static final Set<BlockPos> water = new HashSet<>();
	private static final Set<BlockPos> waterChanges = new HashSet<>();

	private WorldBridge() {
	}

	static void attach(final MinecraftServer s) {
		server = s;
	}

	static void detach() {
		server = null;
		barriers.clear();
		water.clear();
		waterChanges.clear();
	}

	public static boolean ready() {
		return server != null;
	}

	static MinecraftServer server() {
		return server;
	}

	/** Columns of solid ground from the host: {x, z, yBottom, yTop, ...} in block coordinates (inclusive). Only air is replaced. */
	public static void solid(final int[] columns) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			BlockState barrier = Blocks.BARRIER.defaultBlockState();
			BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();
			placingGround = true;
			for (int i = 0; i + 3 < columns.length; i += 4) {
				for (int y = columns[i + 2]; y <= columns[i + 3]; y++) {
					pos.set(columns[i], y, columns[i + 1]);
					if (level.isInWorldBounds(pos) && level.getBlockState(pos).isAir()) {
						level.setBlock(pos, barrier, Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
						barriers.add(pos.immutable());
					}
				}
			}

			placingGround = false;
		});
	}

	/** Remove the host's ground (e.g. when the host teleports somewhere else): see removeGround. */
	public static void clearSolid() {
		MinecraftServer s = server;
		if (s != null) {
			s.execute(() -> removeGround(s));
		}
	}

	/**
	 * Every barrier we placed, and any other around the player: the host's ground from earlier sessions (saved with
	 * the world) would otherwise stay wherever it was, at heights that are wrong now. Server thread.
	 */
	static void removeGround(final MinecraftServer s) {
		ServerLevel level = s.overworld();
		BlockState air = Blocks.AIR.defaultBlockState();
		placingGround = true;
		for (BlockPos pos : barriers) {
			if (level.getBlockState(pos).is(Blocks.BARRIER)) {
				level.setBlock(pos, air, Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
			}
		}

		barriers.clear();
		if (!s.getPlayerList().getPlayers().isEmpty()) {
			BlockPos c = s.getPlayerList().getPlayers().get(0).blockPosition();
			BlockPos.MutableBlockPos p = new BlockPos.MutableBlockPos();
			for (int cx = (c.getX() - 80) >> 4; cx <= (c.getX() + 80) >> 4; cx++) {
				for (int cz = (c.getZ() - 80) >> 4; cz <= (c.getZ() + 80) >> 4; cz++) {
					LevelChunk chunk = level.getChunkSource().getChunkNow(cx, cz);
					if (chunk == null) {
						continue;
					}

					LevelChunkSection[] sections = chunk.getSections();
					for (int i = 0; i < sections.length; i++) {
						LevelChunkSection section = sections[i];
						if (section.hasOnlyAir() || !section.getStates().maybeHas(state -> state.is(Blocks.BARRIER))) {
							continue;
						}

						int y0 = level.getSectionYFromSectionIndex(i) << 4;
						for (int x = 0; x < 16; x++) {
							for (int y = 0; y < 16; y++) {
								for (int z = 0; z < 16; z++) {
									if (section.getBlockState(x, y, z).is(Blocks.BARRIER)) {
										level.setBlock(p.set((cx << 4) + x, y0 + y, (cz << 4) + z), air, Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
									}
								}
							}
						}
					}
				}
			}
		}

		placingGround = false;
	}

	/** Run a command as the server (op). Results go to the log, not to chat (send_command_feedback is off). */
	public static void command(final String command) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			Passthrough.LOG.info("command: {}", command);
			s.getCommands().performPrefixedCommand(s.createCommandSourceStack(), command);
		});
	}

	private static boolean solidForHost(final ServerLevel level, final BlockPos pos, final BlockState state) {
		// the Nether's and the End's ground is the host's own ground turned: nothing to collide with that isn't there
		// already; and an end portal's frame is stepped over, not climbed (the host's player walks into the portal)
		return !state.isAir() && !state.is(Blocks.BARRIER) && !state.is(Blocks.END_PORTAL_FRAME) && !state.getCollisionShape(level, pos).isEmpty()
			&& !Nether.isGround(pos) && !TheEnd.isGround(pos);
	}

	/** Arrows the host already hit something with (they stay where they hit and aren't reported again). */
	private static final String HIT_TAG = "passthrough_hit";

	/** Every server tick: block changes, and projectiles in flight for the host to trace through its own world. */
	static void tick(final MinecraftServer s) {
		flush(s);
		flushWater(s.overworld());
		if (Passthrough.active) {
			reportProjectiles(s.overworld());
		}

		MobWar.tick(s);
		Nether.tick(s);
		TheEnd.tick(s);
	}

	/** Whether the last tick reported any projectiles (when they're all gone the host hears so once). */
	private static boolean reportedProjectiles;

	/**
	 * Steve's projectiles in flight (arrows, tridents, crossbow fireworks, snowballs, eggs, potions, wind charges, ender
	 * pearls, the fishing hook): {"t":"proj","p":[[id,kind,x,y,z],...]}, and once {"t":"proj","p":[]} when none are left.
	 * Mobs' arrows aren't traced by the host: they hit its people's proxies here.
	 */
	private static void reportProjectiles(final ServerLevel level) {
		StringBuilder b = null;
		for (Entity e : level.getAllEntities()) {
			if (!(e instanceof Projectile projectile) || !(projectile.getOwner() instanceof Player)) {
				continue;
			}

			String kind = projectileKind(e);
			if (kind != null) {
				b = b == null ? new StringBuilder("{\"t\":\"proj\",\"p\":[") : b.append(',');
				b.append(String.format(Locale.ROOT, "[%d,\"%s\",%.3f,%.3f,%.3f]", e.getId(), kind, e.getX(), e.getY(), e.getZ()));
			}
		}

		if (b != null) {
			Passthrough.events.accept(b.append("]}").toString());
		} else if (reportedProjectiles) {
			Passthrough.events.accept("{\"t\":\"proj\",\"p\":[]}");
		}

		reportedProjectiles = b != null;
	}

	/** How the host traces a player's projectile ("kind" or "kind:detail"), or null for one it leaves alone. */
	private static String projectileKind(final Entity e) {
		boolean moving = e.getDeltaMovement().lengthSqr() > 1.0E-4;
		if (e instanceof ThrownTrident trident) {
			return trident.entityTags().contains(HIT_TAG) || !moving ? null : channeling(trident.getWeaponItem()) ? "trident:chan" : "trident";
		} else if (e instanceof AbstractArrow arrow) {
			return arrow.entityTags().contains(HIT_TAG) || !moving ? null : "arrow";
		} else if (e instanceof FireworkRocketEntity rocket) {
			return rocket.isShotAtAngle() ? "firework" : null; // not the ones boosting an elytra flight
		} else if (e instanceof Snowball) {
			return "snowball";
		} else if (e instanceof ThrownEgg) {
			return "egg";
		} else if (e instanceof AbstractThrownPotion potion) {
			return "potion:" + potionName(potion.getItem());
		} else if (e instanceof WindCharge) {
			return "wind";
		} else if (e instanceof ThrownEnderpearl) {
			return "pearl"; // the host's walls, floors and ceilings stop it wherever it flies (it teleports there)
		} else if (e instanceof FishingHook) {
			return moving ? "bobber" : null;
		}

		return null;
	}

	private static boolean channeling(final ItemStack weapon) {
		return weapon != null && weapon.getEnchantments().keySet().stream().anyMatch(enchantment -> enchantment.is(Enchantments.CHANNELING));
	}

	/** The potion's name ("strong_harming", "healing", ...), or "". */
	private static String potionName(final ItemStack stack) {
		PotionContents contents = stack.get(DataComponents.POTION_CONTENTS);
		return contents == null ? "" : contents.potion().flatMap(Holder::unwrapKey).map(key -> key.identifier().getPath()).orElse("");
	}

	/**
	 * The host traced a projectile into something of its own: a firework bursts there; an arrow goes into a person
	 * or car (gone) or sticks where it hit a wall; an ender pearl lands there (Steve teleports).
	 */
	public static void projectileHit(final int id, final double x, final double y, final double z, final boolean stick) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			Entity e = level.getEntity(id);
			if (e instanceof FireworkRocketEntity) {
				e.setPos(x, y, z);
				level.broadcastEntityEvent(e, (byte) 17);
				e.discard();
			} else if (e instanceof AbstractArrow) {
				if (stick) {
					e.setPos(x, y, z);
					e.setDeltaMovement(Vec3.ZERO);
					e.setNoGravity(true);
					e.addTag(HIT_TAG);
				} else {
					e.discard();
				}
			} else if (e instanceof Projectile projectile) {
				// a snowball, egg, potion, wind charge, ender pearl or the fishing hook ends there as if it hit a block (a
				// potion splashes, a wind charge bursts, an egg may hatch, a pearl teleports Steve there)
				e.setPos(x, y, z);
				e.setOldPosAndRot();
				((ProjectileInvoker) projectile).passthrough$onHit(new BlockHitResult(new Vec3(x, y, z), Direction.UP, BlockPos.containing(x, y, z), false));
				if (!e.isRemoved()) {
					e.discard();
				}
			}
		});
	}

	/** Server thread, from Level.setBlock: remember the change; flushed once per tick. */
	public static void onBlockChanged(final ServerLevel level, final BlockPos pos, final BlockState state) {
		if (!Passthrough.active || level != level.getServer().overworld()) {
			return;
		}

		Nether.onBlockChanged(pos, state);
		if (!placingGround) {
			changes.put(pos.immutable(), solidForHost(level, pos, state));
		}

		if (isWater(state.getFluidState()) || water.contains(pos)) {
			waterChanges.add(pos.immutable());
		}
	}

	private static boolean isWater(final FluidState fluid) {
		return fluid.is(FluidTags.WATER);
	}

	/**
	 * End of each server tick: water that came or went (and the water beside it, whose flow changes with it), as
	 * {"t":"water","set":[x,y,z,flowX,flowZ,...],"clear":[x,y,z,...]} (flow in thousandths).
	 */
	private static void flushWater(final ServerLevel level) {
		if (waterChanges.isEmpty()) {
			return;
		}

		Set<BlockPos> report = new HashSet<>(waterChanges);
		for (BlockPos p : waterChanges) {
			for (Direction d : Direction.Plane.HORIZONTAL) {
				if (water.contains(p.relative(d))) {
					report.add(p.relative(d));
				}
			}
		}

		waterChanges.clear();
		StringBuilder set = new StringBuilder();
		StringBuilder clear = new StringBuilder();
		for (BlockPos p : report) {
			FluidState fluid = level.getFluidState(p);
			if (isWater(fluid)) {
				water.add(p);
				Vec3 flow = fluid.getFlow(level, p);
				set.append(set.isEmpty() ? "" : ",").append(p.getX()).append(',').append(p.getY()).append(',').append(p.getZ())
					.append(',').append(Math.round(flow.x * 1000.0)).append(',').append(Math.round(flow.z * 1000.0));
			} else if (water.remove(p)) {
				clear.append(clear.isEmpty() ? "" : ",").append(p.getX()).append(',').append(p.getY()).append(',').append(p.getZ());
			}
		}

		if (!set.isEmpty() || !clear.isEmpty()) {
			Passthrough.events.accept("{\"t\":\"water\",\"set\":[" + set + "],\"clear\":[" + clear + "]}");
		}
	}

	/** While on, block changes are the host's own ground being edited (not reported as blocks to collide with). */
	static void quietGround(final boolean on) {
		placingGround = on;
	}

	/** End of each server tick: {"t":"blocks","set":[x,y,z,...],"clear":[x,y,z,...]}. */
	static void flush(final MinecraftServer s) {
		if (changes.isEmpty()) {
			return;
		}

		StringBuilder set = new StringBuilder();
		StringBuilder clear = new StringBuilder();
		for (Map.Entry<BlockPos, Boolean> e : changes.entrySet()) {
			StringBuilder b = e.getValue() ? set : clear;
			BlockPos p = e.getKey();
			b.append(b.isEmpty() ? "" : ",").append(p.getX()).append(',').append(p.getY()).append(',').append(p.getZ());
		}

		changes.clear();
		Passthrough.events.accept("{\"t\":\"blocks\",\"set\":[" + set + "],\"clear\":[" + clear + "]}");
	}

	/** Every solid block within `radius` of the player, as one "blocks" message (the host's props start from this). */
	public static void sync(final int radius) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			ServerPlayer player = s.getPlayerList().getPlayers().isEmpty() ? null : s.getPlayerList().getPlayers().get(0);
			if (player == null) {
				return;
			}

			BlockPos c = player.blockPosition();
			BlockPos.MutableBlockPos p = new BlockPos.MutableBlockPos();
			int found = 0;
			for (int x = -radius; x <= radius && found < 3000; x++) {
				for (int z = -radius; z <= radius && found < 3000; z++) {
					for (int y = -24; y <= 40; y++) {
						p.set(c.getX() + x, c.getY() + y, c.getZ() + z);
						if (!level.isInWorldBounds(p) || !level.isLoaded(p)) {
							continue;
						}

						BlockState state = level.getBlockState(p);
						if (solidForHost(level, p, state)) {
							changes.put(p.immutable(), true);
							found++;
						} else if (isWater(state.getFluidState()) && water.size() < 4000) {
							waterChanges.add(p.immutable());
						}
					}
				}
			}

			flush(s);
			water.clear();
			flushWater(level);
		});
	}

	/** Whether glide() put the elytra on (and takes it off again when the flight ends). */
	private static boolean equippedByGlide;

	/**
	 * Gliding on the elytra (creative flight off), launched forward along the look; or back to creative flight. Only
	 * with an elytra on, unless `equip` (the director's flights) puts one on for the flight.
	 */
	public static void glide(final boolean start, final double speed, final boolean equip) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			if (s.getPlayerList().getPlayers().isEmpty()) {
				return;
			}

			ServerPlayer player = s.getPlayerList().getPlayers().get(0);
			if (start) {
				if (!player.getItemBySlot(EquipmentSlot.CHEST).has(DataComponents.GLIDER)) {
					if (!equip) {
						return; // no elytra on: no flight
					}

					player.setItemSlot(EquipmentSlot.CHEST, new ItemStack(Items.ELYTRA));
					equippedByGlide = true;
				}

				player.getAbilities().flying = false;
				player.onUpdateAbilities();
				player.setOnGround(false);
				player.startFallFlying();
				player.setDeltaMovement(player.getLookAngle().scale(speed));
				player.needsSync = true;
			} else {
				player.stopFallFlying();
				if (equippedByGlide) {
					player.setItemSlot(EquipmentSlot.CHEST, ItemStack.EMPTY);
					equippedByGlide = false;
				}

				player.getAbilities().flying = true;
				player.onUpdateAbilities();
			}
		});
	}

	/** `source`: what exploded or was blown up, e.g. "tnt", "creeper", "fireball" (a ghast's). */
	public static void onExplosion(final Vec3 center, final float radius, final String source) {
		if (Passthrough.active) {
			Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"explosion\",\"pos\":[%.3f,%.3f,%.3f],\"r\":%.2f,\"src\":\"%s\"}",
				center.x, center.y, center.z, radius, source));
		}
	}
}
