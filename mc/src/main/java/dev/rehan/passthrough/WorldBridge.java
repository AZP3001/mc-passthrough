package dev.rehan.passthrough;

import dev.rehan.passthrough.mixin.AbstractArrowAccessor;
import dev.rehan.passthrough.mixin.ProjectileInvoker;
import java.util.HashMap;
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
import net.minecraft.world.level.Level;
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
					if (level.isInWorldBounds(pos) && level.getBlockState(pos).isAir() && !HostDig.keepOut(pos)) {
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
		command(command, false);
	}

	/** A command run on the server; `chat`: as the player's own, passed on to the host as typed ones are. */
	public static void command(final String command, final boolean chat) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			Passthrough.LOG.info("command: {}", command);
			// (the mod's own: not passed on to the host, whose clock and weather a setup "time set noon" reset)
			HostBridge.internal = !chat;
			try {
				s.getCommands().performPrefixedCommand(s.createCommandSourceStack(), command);
			} finally {
				HostBridge.internal = false;
			}
		});
	}

	private static boolean solidForHost(final ServerLevel level, final BlockPos pos, final BlockState state) {
		// the Nether's and the End's ground is the host's own ground turned: nothing to collide with that isn't there
		// already; and an end portal's frame is stepped over, not climbed (the host's player walks into the portal)
		// (nor a block filling round a hole mined in the host's world: the host's own wall or ground is there)
		return !state.isAir() && !state.is(Blocks.BARRIER) && !state.is(Blocks.END_PORTAL_FRAME) && !state.getCollisionShape(level, pos).isEmpty()
			&& !Nether.isGround(pos) && !TheEnd.isGround(pos) && !HostDig.isFill(pos);
	}

	/** Arrows the host already hit something with (they stay where they hit and aren't reported again). */
	private static final String HIT_TAG = "passthrough_hit";

	/**
	 * Minecraft's player killed one of the host's people or animals (a swing, an arrow, a trident): experience orbs
	 * where they fell, as a mob's would drop. Any thread.
	 */
	public static void experience(final double x, final double y, final double z, final int amount) {
		MinecraftServer s = server;
		if (s == null || amount <= 0) {
			return;
		}

		s.execute(() -> net.minecraft.world.entity.ExperienceOrb.award(s.overworld(), new Vec3(x, y + 0.5, z), Math.min(amount, 100)));
	}

	/** The host's map waypoint (Minecraft coordinates), where compasses point, or null. Any thread. */
	private static volatile BlockPos waypoint;

	/** The host's waypoint moved (or went: null): compasses point there, as a lodestone compass would. Any thread. */
	public static void waypoint(final BlockPos at) {
		waypoint = at;
	}

	/**
	 * The player's compasses (not one tied to a lodestone) point at the host's waypoint. (Setting the world's spawn there
	 * moved the player's respawn point too.)
	 */
	private static void compasses(final MinecraftServer s) {
		BlockPos at = waypoint;
		if (at == null || s.getPlayerList().getPlayers().isEmpty()) {
			return;
		}

		net.minecraft.world.item.component.LodestoneTracker want = new net.minecraft.world.item.component.LodestoneTracker(
			java.util.Optional.of(net.minecraft.core.GlobalPos.of(net.minecraft.world.level.Level.OVERWORLD, at)), false);
		ServerPlayer player = s.getPlayerList().getPlayers().get(0);
		net.minecraft.world.entity.player.Inventory inventory = player.getInventory();
		for (int i = 0; i < inventory.getContainerSize(); i++) {
			ItemStack stack = inventory.getItem(i);
			if (!stack.is(Items.COMPASS)) {
				continue;
			}

			net.minecraft.world.item.component.LodestoneTracker now = stack.get(DataComponents.LODESTONE_TRACKER);
			if (now == null || (!now.tracked() && !want.equals(now))) {
				stack.set(DataComponents.LODESTONE_TRACKER, want);
			}
		}
	}

	/** Every server tick: block changes, and projectiles in flight for the host to trace through its own world. */
	static void tick(final MinecraftServer s) {
		flush(s);
		flushWater(s.overworld());
		if (Passthrough.active && s.getTickCount() % 20 == 0) {
			compasses(s);
		}
		if (Passthrough.active) {
			reportProjectiles(s.overworld());
		}

		MobWar.tick(s);
		Leads.tick(s);
		HostDig.tick(s);
		Nether.tick(s);
		TheEnd.tick(s);
		HostBridge.tick(s);
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

	/** The level of an enchantment (its id's path) on an item, or 0. */
	static int enchantment(final ItemStack stack, final String id) {
		for (var en : stack.getEnchantments().entrySet()) {
			if (en.getKey().unwrapKey().map(k -> k.identifier().getPath().equals(id)).orElse(false)) {
				return en.getIntValue();
			}
		}

		return 0;
	}

	/** How the host traces a player's projectile ("kind" or "kind:detail"), or null for one it leaves alone. */
	private static String projectileKind(final Entity e) {
		boolean moving = e.getDeltaMovement().lengthSqr() > 1.0E-4;
		if (e instanceof ThrownTrident trident) {
			if (trident.entityTags().contains(HIT_TAG) || !moving) {
				return null;
			}

			// channeling (lightning where it lands), impaling (harder on what's in water or rain)
			ItemStack weapon = trident.getWeaponItem();
			int impaling = weapon == null ? 0 : enchantment(weapon, "impaling");
			return "trident" + (channeling(weapon) ? ":chan" : "") + (impaling > 0 ? ":im" + impaling : "");
		} else if (e instanceof AbstractArrow arrow) {
			// the bow's enchantments go along: power, punch, flame (a burning arrow)
			if (arrow.entityTags().contains(HIT_TAG) || !moving) {
				return null;
			}

			ItemStack bow = arrow.getWeaponItem();
			int power = bow == null ? 0 : enchantment(bow, "power"), punch = bow == null ? 0 : enchantment(bow, "punch");
			return "arrow" + (power > 0 ? ":pw" + power : "") + (punch > 0 ? ":pu" + punch : "") + (arrow.isOnFire() ? ":fl" : "");
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

	/** The host's player was saved from dying by the totem in Minecraft's player's hand: used up, as Minecraft's is. */
	public static void totem() {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			if (s.getPlayerList().getPlayers().isEmpty()) {
				return;
			}

			ServerPlayer player = s.getPlayerList().getPlayers().get(0);
			for (net.minecraft.world.InteractionHand hand : net.minecraft.world.InteractionHand.values()) {
				net.minecraft.world.item.ItemStack held = player.getItemInHand(hand);
				if (held.is(net.minecraft.world.item.Items.TOTEM_OF_UNDYING)) {
					held.shrink(1);
					break;
				}
			}

			player.addEffect(new net.minecraft.world.effect.MobEffectInstance(net.minecraft.world.effect.MobEffects.REGENERATION, 900, 1));
			player.addEffect(new net.minecraft.world.effect.MobEffectInstance(net.minecraft.world.effect.MobEffects.ABSORPTION, 100, 1));
			player.addEffect(new net.minecraft.world.effect.MobEffectInstance(net.minecraft.world.effect.MobEffects.FIRE_RESISTANCE, 800, 0));
			player.level().broadcastEntityEvent(player, (byte) 35); // (the totem's animation, sound and particles)
		});
	}

	/**
	 * Arrows stuck in the host's people, cars and signs, where those are now ({id, x, y, z, yaw, pitch} each), and those
	 * to let go of (gone: they fall out). Any thread.
	 */
	public static void stuckArrows(final double[][] at, final int[] gone) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			for (double[] a : at) {
				if (level.getEntity((int) a[0]) instanceof AbstractArrow arrow) {
					arrow.setPos(a[1], a[2], a[3]);
					arrow.setYRot((float) a[4]);
					arrow.setXRot((float) a[5]);
				}
			}

			for (int id : gone) {
				Entity e = level.getEntity(id);
				if (e instanceof AbstractArrow) {
					e.discard();
				}
			}
		});
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
			} else if (e instanceof AbstractArrow arrow) {
				if (stick) {
					// stuck in the host's wall or ground: in the ground on both sides (only moved there and left floating,
					// the client's copy flew on and fell, and the two fought over where it was: it jittered about)
					e.setPos(x, y, z);
					e.setDeltaMovement(Vec3.ZERO);
					AbstractArrowAccessor stuck = (AbstractArrowAccessor) arrow;
					stuck.passthrough$setLastState(level.getBlockState(BlockPos.containing(x, y, z)));
					stuck.passthrough$setInGround(true);
					e.addTag(HIT_TAG);
					e.needsSync = true;
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
		HostDig.onBlockChanged(pos, state);
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
			// by chunk section, skipping those with nothing but air and the host's ground (barriers) in them: the whole
			// box block by block was about a million lookups in one tick (a hitch on every relevel and connect)
			int yMin = c.getY() - 24, yMax = c.getY() + 40;
			for (int cx = (c.getX() - radius) >> 4; cx <= (c.getX() + radius) >> 4 && found < 3000; cx++) {
				for (int cz = (c.getZ() - radius) >> 4; cz <= (c.getZ() + radius) >> 4 && found < 3000; cz++) {
					LevelChunk chunk = level.getChunkSource().getChunkNow(cx, cz);
					if (chunk == null) {
						continue;
					}

					LevelChunkSection[] sections = chunk.getSections();
					for (int i = 0; i < sections.length && found < 3000; i++) {
						int y0 = level.getSectionYFromSectionIndex(i) << 4;
						LevelChunkSection section = sections[i];
						if (y0 + 15 < yMin || y0 > yMax || section.hasOnlyAir()
							|| !section.getStates().maybeHas(state -> !state.isAir() && !state.is(Blocks.BARRIER))) {
							continue;
						}

						for (int x = 0; x < 16; x++) {
							int bx = (cx << 4) + x;
							if (Math.abs(bx - c.getX()) > radius) {
								continue;
							}

							for (int z = 0; z < 16; z++) {
								int bz = (cz << 4) + z;
								if (Math.abs(bz - c.getZ()) > radius) {
									continue;
								}

								for (int y = Math.max(0, yMin - y0); y <= Math.min(15, yMax - y0); y++) {
									BlockState state = section.getBlockState(x, y, z);
									if (state.isAir() || state.is(Blocks.BARRIER)) {
										continue;
									}

									p.set(bx, y0 + y, bz);
									if (solidForHost(level, p, state)) {
										changes.put(p.immutable(), true);
										found++;
									} else if (isWater(state.getFluidState()) && water.size() < 4000) {
										waterChanges.add(p.immutable());
									}
								}
							}
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
	/** Marks the elytra glide() put on (only that one is ever taken off: never the player's own chestplate or elytra). */
	public static final String GLIDE_TAG = "passthrough_glide";
	/** Whether the player was in creative flight when the glide began (it's back in it after, only then). */
	private static boolean flyingBeforeGlide;

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

					ItemStack elytra = new ItemStack(Items.ELYTRA);
					net.minecraft.world.item.component.CustomData.update(DataComponents.CUSTOM_DATA, elytra, tag -> tag.putBoolean(GLIDE_TAG, true));
					player.setItemSlot(EquipmentSlot.CHEST, elytra);
					equippedByGlide = true;
				}

				flyingBeforeGlide = player.getAbilities().flying;
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

				// (back in creative flight only if it was flying before, and may: never left flying in survival)
				player.getAbilities().flying = flyingBeforeGlide && player.getAbilities().mayfly;
				player.onUpdateAbilities();
			}
		});
	}

	/** How shot up each block is (1: broken), from the host's guns. Server thread. */
	private static final Map<BlockPos, Float> shotDamage = new HashMap<>();
	/** The host's own blast going off in Minecraft's world: not reported back to it (it set its own off already). */
	private static boolean hostBlast;

	/**
	 * A shot from the host's guns landed at (x, y, z), flying along dir (Minecraft coordinates): the block it hit takes
	 * damage, by how hard it is (glass and leaves break at once, stone after a few, obsidian practically never); a
	 * rocket's or grenade's (boom) blows the blocks round it up. Any thread.
	 */
	public static void hostShot(final double x, final double y, final double z, final double dx, final double dy, final double dz, final boolean boom) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			if (boom) {
				hostBlast = true;
				try {
					// (the blocks and Minecraft's mobs, not the player: the host's own blast hurt them already)
					level.explode(null, null, new net.minecraft.world.level.ExplosionDamageCalculator() {
						@Override
						public boolean shouldDamageEntity(final net.minecraft.world.level.Explosion explosion, final Entity entity) {
							return !(entity instanceof net.minecraft.world.entity.player.Player);
						}
					}, new Vec3(x, y, z), 2.5F, false, Level.ExplosionInteraction.TNT);
				} finally {
					hostBlast = false;
				}

				return;
			}

			// the block just inside where it hit
			for (double in = 0.05; in <= 0.6; in += 0.15) {
				BlockPos pos = BlockPos.containing(x + dx * in, y + dy * in, z + dz * in);
				BlockState state = level.getBlockState(pos);
				if (state.isAir() || state.is(Blocks.BARRIER) || !state.getFluidState().isEmpty()) {
					continue; // (barriers: the host's own walls)
				}

				float hardness = state.getDestroySpeed(level, pos);
				if (hardness < 0.0F) {
					return; // bedrock and the like
				}

				float damage = shotDamage.getOrDefault(pos, 0.0F) + 1.0F / Math.max(1.0F, hardness * 2.5F);
				if (damage >= 0.999F) {
					shotDamage.remove(pos);
					level.destroyBlockProgress(pos.hashCode(), pos, -1);
					level.destroyBlock(pos, true);
				} else {
					shotDamage.put(pos.immutable(), damage);
					level.destroyBlockProgress(pos.hashCode(), pos, (int) (damage * 10.0F));
				}

				if (shotDamage.size() > 512) {
					shotDamage.clear();
				}

				return;
			}
		});
	}

	/** An explosion in Minecraft's world, for the host. `source`: what exploded or was blown up, e.g. "tnt", "creeper", "fireball" (a ghast's). */
	public static void onExplosion(final Vec3 center, final float radius, final String source) {
		if (Passthrough.active && !hostBlast) {
			Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"explosion\",\"pos\":[%.3f,%.3f,%.3f],\"r\":%.2f,\"src\":\"%s\"}",
				center.x, center.y, center.z, radius, source));
		}
	}
}
