package dev.rehan.passthrough;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.Holder;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.ai.attributes.Attribute;
import net.minecraft.world.entity.ai.attributes.AttributeInstance;
import net.minecraft.world.entity.ai.attributes.Attributes;
import net.minecraft.world.entity.decoration.ArmorStand;
import net.minecraft.world.entity.player.Abilities;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.level.block.BaseFireBlock;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.chunk.LevelChunkSection;
import net.minecraft.world.phys.AABB;

/**
 * More of the host's world and Minecraft's together:
 * <ul>
 * <li>Minecraft's animals that the host has too (a cow, a pig, a chicken, ...) turn into the host's own when they appear;</li>
 * <li>Minecraft's mobs near the player go to the host ("allmobs"), which kills those its cars and flung people hit
 * ("mobkill"), sets alight those its fires touch ("mobfire"), and sends its own fires ("gtafire");</li>
 * <li>the player reaches 8 blocks and moves faster; in survival its health is the host's player's, and the damage it
 * takes goes to the host ("pdmg");</li>
 * <li>commands: /spectator, /creative, /clearall, and /kill, /time and /weather reach the host's world too.</li>
 * </ul>
 */
public final class HostBridge {
	/** Minecraft's animals the host has too, by entity id path: the host's model. */
	private static final Map<String, String> ANIMALS = Map.ofEntries(
		Map.entry("cow", "a_c_cow"), Map.entry("mooshroom", "a_c_cow"), Map.entry("pig", "a_c_pig"), Map.entry("chicken", "a_c_hen"),
		Map.entry("rabbit", "a_c_rabbit_01"), Map.entry("cat", "a_c_cat_01"), Map.entry("wolf", "a_c_husky"), Map.entry("fox", "a_c_coyote"),
		Map.entry("ocelot", "a_c_mtlion"), Map.entry("dolphin", "a_c_dolphin"), Map.entry("cod", "a_c_fish"), Map.entry("salmon", "a_c_fish"),
		Map.entry("tropical_fish", "a_c_fish")
	);
	/**
	 * Damage past Minecraft's player's last health point (a long fall, a creeper at close range): Minecraft's player is
	 * kept at 1 (the host decides when the player dies), and this much more goes to the host with the rest. Server thread.
	 */
	public static float overkill;

	/** The player's reach in blocks (/range). */
	public static volatile double reach = 8.0;
	private static final double SPEED = 0.14; // Minecraft's own is 0.1
	private static final List<Entity> toConvert = new ArrayList<>();
	private static boolean mobsSent;
	private static float healthSet = -1.0F;
	private static boolean attributesSet;

	private HostBridge() {
	}

	/** The host's model for a Minecraft entity type (its id's path), or null. */
	public static String animal(final String path) {
		return ANIMALS.get(path);
	}

	/** Server thread: an entity appeared. One of the shared animals turns into the host's own (next tick). */
	static void onEntityLoad(final Entity e, final ServerLevel level) {
		if (Passthrough.active && level == level.getServer().overworld() && ANIMALS.containsKey(path(e)) && !kept(e)) {
			toConvert.add(e);
		}
	}

	/** One the player cares about (tamed, named, on a lead, ridden, a horse or llama tamed): stays Minecraft's own. */
	private static boolean kept(final Entity e) {
		return e.hasCustomName() || e.isVehicle() || e.isPassenger()
			|| (e instanceof net.minecraft.world.entity.OwnableEntity o && o.getOwnerReference() != null)
			|| (e instanceof net.minecraft.world.entity.Leashable l && l.isLeashed())
			|| (e instanceof net.minecraft.world.entity.animal.equine.AbstractHorse h && h.isTamed());
	}

	private static String path(final Entity e) {
		return BuiltInRegistries.ENTITY_TYPE.getKey(e.getType()).getPath();
	}

	/** Every server tick. */
	static void tick(final MinecraftServer s) {
		ServerLevel level = s.overworld();
		ServerPlayer player = s.getPlayerList().getPlayers().isEmpty() ? null : s.getPlayerList().getPlayers().get(0);
		if (!toConvert.isEmpty()) {
			for (Entity e : toConvert) {
				if (!e.isRemoved() && Passthrough.active) {
					Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"animal\",\"m\":\"%s\",\"type\":28,\"pos\":[%.3f,%.3f,%.3f],\"yaw\":%.1f}",
						ANIMALS.get(path(e)), e.getX(), e.getY(), e.getZ(), e.getYRot()));
					e.discard();
				}
			}

			toConvert.clear();
		}

		if (player == null) {
			return;
		}

		playerTick(player);
		if (Passthrough.active && s.getTickCount() % 5 == 0 && player.level() == level) {
			reportMobs(level, player);
		}
	}

	/** The player's reach (8 blocks), speed, and in survival its health (the host's). */
	private static void playerTick(final ServerPlayer player) {
		boolean on = Passthrough.active;
		if (on || attributesSet) {
			base(player, Attributes.BLOCK_INTERACTION_RANGE, on ? reach : 4.5);
			base(player, Attributes.ENTITY_INTERACTION_RANGE, on ? reach : 3.0);
			base(player, Attributes.MOVEMENT_SPEED, on ? SPEED : 0.1);
			attributesSet = on;
		}

		// the host's breath (while it moves the player): Minecraft's air bubbles are it, and Minecraft never drowns its
		// player itself on top of the host's own drowning
		float air = Passthrough.hostAir;
		if (on && air >= 0.0F && !Passthrough.walking) {
			player.setAirSupply(Math.round(air * player.getMaxAirSupply()));
		}

		// a fall while the host moves the player (its own movement: a jump, a skydive) is the host's to hurt for
		if (on && !Passthrough.walking) {
			player.resetFallDistance();
		}

		// creative while a host is attached: Minecraft's survival dangers all the same (mobs hunt the player, fire burns,
		// water drowns, falls hurt: the host's health takes it), with creative's building and inventory kept; never
		// hungry (the hunger bar is the host's stamina)
		if (player.isCreative()) {
			Abilities abilities = player.getAbilities();
			if (abilities.invulnerable == on) {
				abilities.invulnerable = !on;
				player.onUpdateAbilities();
			}

			if (on && player.getFoodData().getFoodLevel() < 20) {
				player.getFoodData().setFoodLevel(20);
			}
		}

		float host = Passthrough.hostHealth;
		if (!on || host < 0.0F || player.isSpectator() || !player.isAlive()) {
			healthSet = -1.0F;
			overkill = 0.0F;
			return;
		}

		// what Minecraft did to its player since (a mob's hit, a fall, drowning) goes to the host's player
		if (healthSet > 0.0F && (player.getHealth() < healthSet - 0.01F || overkill > 0.0F)) {
			Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"pdmg\",\"d\":%.2f}", Math.max(0.0F, healthSet - player.getHealth()) + overkill));
			overkill = 0.0F;
		} else if (healthSet > 0.0F && player.getHealth() > healthSet + 0.01F) {
			// and what healed it (a potion, regeneration, food) heals the host's player
			Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"pheal\",\"d\":%.2f}", player.getHealth() - healthSet));
		}

		float want = Math.max(1.0F, host); // (the host decides when the player dies)
		if (Math.abs(player.getHealth() - want) > 0.01F) {
			player.setHealth(want);
		}

		healthSet = player.getHealth();
	}

	private static void base(final Player player, final Holder<Attribute> attribute, final double value) {
		AttributeInstance a = player.getAttribute(attribute);
		if (a != null && a.getBaseValue() != value) {
			a.setBaseValue(value);
		}
	}

	/** {"t":"allmobs","m":[[id,x,y,z,width,height,onFire],...]}: every mob within 64 blocks of the player. */
	private static void reportMobs(final ServerLevel level, final ServerPlayer player) {
		StringBuilder out = new StringBuilder("{\"t\":\"allmobs\",\"m\":[");
		int n = 0;
		for (Entity e : level.getEntities(player, player.getBoundingBox().inflate(64.0), e -> e instanceof LivingEntity && !(e instanceof Player)
			&& !(e instanceof ArmorStand) && !MobWar.isProxy(e) && !Leads.isProxy(e) && e.isAlive())) {
			if (n > 0) {
				out.append(',');
			}

			out.append(String.format(Locale.ROOT, "[%d,%.2f,%.2f,%.2f,%.2f,%.2f,%d]", e.getId(), e.getX(), e.getY(), e.getZ(), e.getBbWidth(), e.getBbHeight(),
				e.isOnFire() ? 1 : 0));
			if (++n >= 120) {
				break;
			}
		}

		if (n > 0 || mobsSent) {
			Passthrough.events.accept(out.append("]}").toString());
		}

		mobsSent = n > 0;
	}

	/** The host's car ran into these mobs, or someone flung into them: they die. Any thread. */
	public static void mobKill(final int[] ids) {
		MinecraftServer s = WorldBridge.server();
		if (s != null) {
			s.execute(() -> {
				ServerLevel level = s.overworld();
				for (int id : ids) {
					if (level.getEntity(id) instanceof LivingEntity mob && mob.isAlive() && !(mob instanceof Player)) {
						mob.hurtServer(level, level.damageSources().flyIntoWall(), 1000.0F);
					}
				}
			});
		}
	}

	/** The host's fire reached these mobs. Any thread. */
	public static void mobFire(final int[] ids) {
		MinecraftServer s = WorldBridge.server();
		if (s != null) {
			s.execute(() -> {
				for (int id : ids) {
					Entity e = s.overworld().getEntity(id);
					if (e != null && !(e instanceof Player)) {
						e.igniteForSeconds(6.0F);
					}
				}
			});
		}
	}

	/** The host's fires near the player ({x, y, z, ...} block cells): fire there too, and mobs in it alight. Any thread. */
	public static void hostFires(final int[] cells) {
		MinecraftServer s = WorldBridge.server();
		if (s == null) {
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			for (int i = 0; i + 2 < cells.length; i += 3) {
				BlockPos pos = new BlockPos(cells[i], cells[i + 1], cells[i + 2]);
				for (BlockPos p : new BlockPos[]{pos, pos.above()}) {
					BlockPos below = p.below();
					if (level.getBlockState(p).isAir() && HostWater.at(p) == null
						&& (level.getBlockState(below).is(Blocks.BARRIER) || level.getBlockState(below).isFaceSturdy(level, below, Direction.UP))) {
						level.setBlock(p, BaseFireBlock.getState(level, p), Block.UPDATE_ALL);
						break;
					}
				}

				for (Entity e : level.getEntities((Entity) null, new AABB(pos).inflate(1.0), e -> e instanceof LivingEntity && !(e instanceof Player))) {
					e.igniteForSeconds(6.0F);
				}
			}
		});
	}

	/**
	 * /clearall: every entity but the players goes, and every block placed around the player (not the host's ground),
	 * the Nether and the End close; the host clears its cars, people and fires around the player too. Server thread.
	 */
	static int clearAll(final MinecraftServer s, final ServerPlayer player) {
		return clearAll(s, player, true);
	}

	/**
	 * The host restarted a mission or loaded a checkpoint: Minecraft's things go (as /clearall), but the host keeps its
	 * own cars and people (the mission's). Any thread.
	 */
	public static void missionRestart() {
		MinecraftServer s = WorldBridge.server();
		if (s != null) {
			s.execute(() -> {
				ServerPlayer player = s.getPlayerList().getPlayers().isEmpty() ? null : s.getPlayerList().getPlayers().get(0);
				if (player != null) {
					int blocks = clearAll(s, player, false);
					player.sendSystemMessage(net.minecraft.network.chat.Component.literal("Mission restarted: Minecraft's things cleared (" + blocks + " blocks)."));
				}
			});
		}
	}

	/** hostToo: the host clears its own cars, people and fires round the player too (/clearall). */
	static int clearAll(final MinecraftServer s, final ServerPlayer player, final boolean hostToo) {
		ServerLevel level = s.overworld();
		Nether.closeIfOpen(level);
		TheEnd.closeIfOpen(level);
		List<Entity> gone = new ArrayList<>();
		for (Entity e : level.getAllEntities()) {
			if (!(e instanceof Player)) {
				gone.add(e);
			}
		}

		gone.forEach(Entity::discard);
		int blocks = 0;
		int cx = player.blockPosition().getX() >> 4, cz = player.blockPosition().getZ() >> 4;
		BlockState air = Blocks.AIR.defaultBlockState();
		BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();
		for (int x = cx - 8; x <= cx + 8; x++) {
			for (int z = cz - 8; z <= cz + 8; z++) {
				LevelChunk chunk = level.getChunkSource().getChunkNow(x, z);
				if (chunk == null) {
					continue;
				}

				LevelChunkSection[] sections = chunk.getSections();
				for (int si = 0; si < sections.length; si++) {
					if (sections[si].hasOnlyAir()) {
						continue;
					}

					int y0 = chunk.getSectionYFromSectionIndex(si) << 4;
					for (int dy = 0; dy < 16; dy++) {
						for (int dx = 0; dx < 16; dx++) {
							for (int dz = 0; dz < 16; dz++) {
								BlockState state = sections[si].getBlockState(dx, dy, dz);
								if (!state.isAir() && !state.is(Blocks.BARRIER)) {
									pos.set((x << 4) + dx, y0 + dy, (z << 4) + dz);
									level.setBlock(pos, air, Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
									blocks++;
								}
							}
						}
					}
				}
			}
		}

		Passthrough.events.accept(hostToo ? "{\"t\":\"gtacmd\",\"c\":\"clearall\"}" : "{\"t\":\"gtacmd\",\"c\":\"clearall\",\"soft\":true}");
		return blocks;
	}

	/**
	 * A command typed in Minecraft (before it runs): /kill with @e reaches the host's people and animals too. And after
	 * it ran: /time and /weather set the host's clock and weather. Server thread.
	 */
	/** A command the mod runs itself (WorldBridge.command): Minecraft's only. Server thread. */
	public static boolean internal;

	public static void command(final MinecraftServer s, final String command, final boolean after) {
		if (!Passthrough.active || internal) {
			return;
		}

		String c = command.startsWith("/") ? command.substring(1) : command;
		String[] words = c.trim().split("\\s+");
		if (words.length == 0) {
			return;
		}

		switch (words[0]) {
			case "kill" -> {
				if (after || words.length < 2 || !words[1].startsWith("@e")) {
					return;
				}

				// @e: everyone; @e[type=cow]: the host's cows; @e[type=villager] or a zombie: its people; type=!x: all but
				// those; distance=..r (or a..b) and limit=n as Minecraft's own (nearest first)
				String args = words[1].length() > 3 && words[1].charAt(2) == '[' ? words[1].substring(3, words[1].length() - (words[1].endsWith("]") ? 1 : 0)) : "";
				String what = "all", not = "";
				double near = 0.0, far = 250.0;
				int limit = Integer.MAX_VALUE;
				for (String arg : args.split(",")) {
					String[] kv = arg.split("=", 2);
					if (kv.length < 2) {
						continue;
					}

					String key = kv[0].trim(), value = kv[1].trim();
					switch (key) {
						case "type" -> {
							boolean but = value.startsWith("!");
							String type = (but ? value.substring(1) : value).replace("minecraft:", "");
							String model = ANIMALS.get(type);
							String host = model != null ? model : type.equals("villager") || type.contains("zombie") ? "people" : "none";
							if (but) {
								not = host; // ("none": nothing of the host's is excluded)
							} else {
								what = host;
							}
						}
						case "distance" -> {
							try {
								int dots = value.indexOf("..");
								if (dots < 0) {
									near = far = Double.parseDouble(value);
								} else {
									near = dots == 0 ? 0.0 : Double.parseDouble(value.substring(0, dots));
									far = dots + 2 >= value.length() ? 250.0 : Double.parseDouble(value.substring(dots + 2));
								}
							} catch (NumberFormatException ignored) {
								what = "none";
							}
						}
						case "limit" -> {
							try {
								limit = Integer.parseInt(value);
							} catch (NumberFormatException ignored) {
								what = "none";
							}
						}
						default -> {
						}
					}
				}

				if (!what.equals("none") && limit > 0) {
					Passthrough.events.accept(String.format(Locale.ROOT,
						"{\"t\":\"gtacmd\",\"c\":\"kill\",\"what\":\"%s\",\"not\":\"%s\",\"r0\":%.2f,\"r\":%.2f,\"limit\":%d}", what,
						not.equals("none") ? "" : not, near, Math.min(far, 250.0), Math.min(limit, 100000)));
				}
			}
			case "time" -> {
				if (!after) {
					return;
				}

				// Minecraft's day starts at 6:00
				long t = Math.floorMod(s.overworld().getOverworldClockTime(), 24000L);
				int minutes = (int) ((t * 60L / 1000L + 6 * 60) % (24 * 60));
				Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"gtacmd\",\"c\":\"time\",\"h\":%d,\"m\":%d}", minutes / 60, minutes % 60));
			}
			case "weather" -> {
				if (after && words.length >= 2) {
					String w = words[1].equals("thunder") ? "thunder" : words[1].equals("rain") ? "rain" : "clear";
					Passthrough.events.accept("{\"t\":\"gtacmd\",\"c\":\"weather\",\"w\":\"" + w + "\"}");
				}
			}
			default -> {
			}
		}
	}
}
