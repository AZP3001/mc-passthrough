package dev.rehan.passthrough;

import com.mojang.brigadier.arguments.DoubleArgumentType;
import com.mojang.brigadier.arguments.StringArgumentType;
import com.mojang.brigadier.arguments.IntegerArgumentType;
import java.util.function.Consumer;
import net.minecraft.commands.arguments.EntityArgument;
import net.minecraft.commands.arguments.ResourceArgument;
import net.minecraft.core.Holder;
import net.minecraft.core.component.DataComponents;
import net.minecraft.core.registries.Registries;
import net.minecraft.nbt.CompoundTag;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.component.CustomData;
import net.minecraft.world.item.enchantment.Enchantment;
import net.minecraft.world.item.enchantment.EnchantmentHelper;
import net.fabricmc.api.ModInitializer;
import net.fabricmc.fabric.api.command.v2.CommandRegistrationCallback;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerEntityEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.minecraft.commands.Commands;
import net.minecraft.network.chat.Component;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.level.GameType;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

public class Passthrough implements ModInitializer {
	public static final String ID = "passthrough";
	public static final Logger LOG = LoggerFactory.getLogger(ID);
	/** True while a host game is driving the camera. The integrated server shares this JVM, so both sides read it. */
	public static volatile boolean active;
	/** True while Minecraft moves the player (on foot) and the host's player follows it; false while the host moves it. */
	public static volatile boolean walking;
	/** /fly: Minecraft's movement flies (Space up, Ctrl down), and a car flies with Space twice; off: neither. */
	public static volatile boolean flyAllowed;
	/** An item's enchantment levels over Minecraft's 255 (/enchant): its custom data, by the enchantment's id path. */
	public static final String OVER_LEVELS = "passthrough_enchants";
	/**
	 * What the host's crosshair is on in its own world within reach (a street, a wall), as {x, y, z, nx, ny, nz} in
	 * Minecraft coordinates, or null: buckets, boats and the like use it like a block's face (the host's surfaces aren't
	 * Minecraft blocks, or only rough barriers).
	 */
	public static volatile double @org.jspecify.annotations.Nullable [] hostHit;
	/** The host's player's health in Minecraft's hearts (0..20), or -1: Minecraft's player's health follows it. */
	public static volatile float hostHealth = -1.0F;
	/** Where events for the host go (JSON lines); the client's HostLink sets it. */
	public static volatile Consumer<String> events = message -> {};

	@Override
	public void onInitialize() {
		ServerLifecycleEvents.SERVER_STARTED.register(WorldBridge::attach);
		ServerLifecycleEvents.SERVER_STOPPING.register(server -> {
			// before the world is saved: the Nether goes back and the fight's mobs go, so none of it is kept
			Nether.detach(server);
			TheEnd.detach(server);
			MobWar.detach(server);
			WorldBridge.removeGround(server); // the host's ground isn't saved with the world either
			WorldBridge.detach();
		});
		// type them in chat (T): a lit nether portal or a ready end portal a few blocks ahead
		CommandRegistrationCallback.EVENT.register((dispatcher, registryAccess, environment) -> {
			dispatcher.register(Commands.literal("netherportal").executes(context -> {
				ServerPlayer player = context.getSource().getPlayerOrException();
				double yaw = Math.toRadians(player.getYRot());
				Nether.buildPortal(player.getX() - Math.sin(yaw) * 5.0, player.getY(), player.getZ() + Math.cos(yaw) * 5.0, player.getYRot());
				player.sendSystemMessage(Component.literal("Nether portal ahead: jump in. Walk back through it to come home."));
				return 1;
			}));
			dispatcher.register(Commands.literal("endportal").executes(context -> {
				ServerPlayer player = context.getSource().getPlayerOrException();
				TheEnd.buildPortal(player);
				player.sendSystemMessage(Component.literal("End portal ahead: walk in. Walk in again to come home."));
				return 1;
			}));
			// spectator as in Minecraft, through the host's world too; /creative back
			dispatcher.register(Commands.literal("spectator").executes(context -> {
				context.getSource().getPlayerOrException().setGameMode(GameType.SPECTATOR);
				return 1;
			}));
			dispatcher.register(Commands.literal("creative").executes(context -> {
				context.getSource().getPlayerOrException().setGameMode(GameType.CREATIVE);
				return 1;
			}));
			// GTA's things: /summon car, truck, tank, plane, heli, boat, bike, npc, cop, or any GTA model name (Minecraft's
			// own mobs still summon as ever)
			dispatcher.register(Commands.literal("summon").then(Commands.argument("gta", StringArgumentType.word())
				.suggests((context, builder) -> {
					for (String k : new String[] {"car", "truck", "tank", "plane", "jet", "heli", "boat", "bike", "bus", "police", "npc", "cop", "soldier"}) {
						if (k.startsWith(builder.getRemainingLowerCase())) {
							builder.suggest(k);
						}
					}

					return builder.buildFuture();
				})
				.executes(context -> {
					events.accept("{\"t\":\"gtacmd\",\"c\":\"summon\",\"what\":\"" + StringArgumentType.getString(context, "gta") + "\"}");
					return 1;
				})));
			// GTA's cheats, abilities and spawning: /gta superjump, /gta explosiveammo, /gta spawn tank, ...
			dispatcher.register(Commands.literal("gta").then(Commands.argument("setting", StringArgumentType.greedyString())
				.suggests((context, builder) -> {
					String typed = builder.getRemainingLowerCase();
					String[] all = {"spawn car", "spawn truck", "spawn tank", "spawn plane", "spawn jet", "spawn heli", "spawn boat", "spawn bike",
						"spawn bus", "spawn police", "spawn npc", "spawn cop", "spawn soldier", "superjump", "fastrun", "fastswim", "explosiveammo",
						"fireammo", "explosivemelee", "slidey", "moon", "slowmo", "infiniteammo", "neverwanted", "onehit", "drunk", "wanted",
						"heal", "armor", "weapons"};
					for (String k : all) {
						if (k.startsWith(typed)) {
							builder.suggest(k);
						}
					}

					return builder.buildFuture();
				})
				.executes(context -> {
					String a = StringArgumentType.getString(context, "setting").replaceAll("[^A-Za-z0-9_ ]", "");
					events.accept("{\"t\":\"gtacmd\",\"c\":\"gta\",\"a\":\"" + a + "\"}");
					return 1;
				})));
			// the reach (blocks, and the host's people and cars): /range 12
			dispatcher.register(Commands.literal("range").then(Commands.argument("blocks", DoubleArgumentType.doubleArg(1.0, 64.0))
				.executes(context -> {
					double r = DoubleArgumentType.getDouble(context, "blocks");
					HostBridge.reach = r;
					events.accept(String.format(java.util.Locale.ROOT, "{\"t\":\"gtacmd\",\"c\":\"range\",\"r\":%.2f}", r));
					context.getSource().sendSystemMessage(Component.literal("Range: " + r + " blocks"));
					return 1;
				})));
			// the car the player sits in: /tune max, /tune stock, /tune engine 3, /tune turbo on, /tune visuals ...
			dispatcher.register(Commands.literal("tune")
				.executes(context -> {
					events.accept("{\"t\":\"gtacmd\",\"c\":\"tune\",\"a\":\"max\"}");
					return 1;
				})
				.then(Commands.argument("what", StringArgumentType.greedyString())
				.suggests((context, builder) -> {
					for (String k : new String[] {"max", "stock", "engine", "brakes", "transmission", "suspension", "armor", "turbo", "visuals",
						"spoiler", "bumpers", "skirts", "exhaust", "hood", "roof", "wheels", "xenon", "tint", "horn", "repair"}) {
						if (k.startsWith(builder.getRemainingLowerCase())) {
							builder.suggest(k);
						}
					}

					return builder.buildFuture();
				})
				.executes(context -> {
					String what = StringArgumentType.getString(context, "what").replaceAll("[^A-Za-z0-9 ]", "");
					events.accept("{\"t\":\"gtacmd\",\"c\":\"tune\",\"a\":\"" + what + "\"}");
					return 1;
				})));
			// /enchant at any level (knockback 1000): no level limit or clash check. Minecraft's items hold at most 255;
			// a higher level is kept beside it (custom data), and the host's world takes that one
			dispatcher.register(Commands.literal("enchant").then(Commands.argument("targets", EntityArgument.entities())
				.then(Commands.argument("enchantment", ResourceArgument.resource(registryAccess, Registries.ENCHANTMENT))
				.then(Commands.argument("level", IntegerArgumentType.integer(0)).executes(context -> {
					Holder<Enchantment> e = ResourceArgument.getEnchantment(context, "enchantment");
					int level = IntegerArgumentType.getInteger(context, "level");
					String id = e.unwrapKey().map(k -> k.identifier().getPath()).orElse("?");
					int done = 0;
					for (Entity t : EntityArgument.getEntities(context, "targets")) {
						if (!(t instanceof LivingEntity living) || living.getMainHandItem().isEmpty()) {
							continue;
						}

						ItemStack stack = living.getMainHandItem();
						EnchantmentHelper.updateEnchantments(stack, m -> m.set(e, Math.min(level, 255)));
						CustomData.update(DataComponents.CUSTOM_DATA, stack, tag -> {
							CompoundTag over = tag.getCompoundOrEmpty(OVER_LEVELS);
							if (level > 255) {
								over.putInt(id, level);
							} else {
								over.remove(id);
							}

							tag.put(OVER_LEVELS, over);
						});
						done++;
					}

					int n = done;
					context.getSource().sendSuccess(() -> Component.literal("Enchanted " + n + " item(s) with " + id + " " + level), true);
					return done;
				})))));
			// to the waypoint set on GTA's map
			dispatcher.register(Commands.literal("waypoint").executes(context -> {
				events.accept("{\"t\":\"gtacmd\",\"c\":\"waypoint\"}");
				return 1;
			}));
			// flying on or off (only on with /fly: a jump tapped twice doesn't start it)
			dispatcher.register(Commands.literal("fly").executes(context -> {
				flyAllowed = !flyAllowed;
				events.accept("{\"t\":\"gtacmd\",\"c\":\"fly\",\"on\":" + flyAllowed + "}");
				context.getSource().sendSystemMessage(Component.literal(flyAllowed ? "Flying on (/fly again for off)" : "Flying off"));
				return 1;
			}));
			// everything gone but the inventory: mobs, items, blocks placed, and the host's cars and people around
			dispatcher.register(Commands.literal("clearall").executes(context -> {
				ServerPlayer player = context.getSource().getPlayer() != null ? context.getSource().getPlayer()
					: context.getSource().getServer().getPlayerList().getPlayers().stream().findFirst().orElse(null);
				if (player == null) {
					return 0;
				}

				int blocks = HostBridge.clearAll(context.getSource().getServer(), player);
				player.sendSystemMessage(Component.literal("Cleared everything around (" + blocks + " blocks)."));
				return 1;
			}));
		});
		ServerEntityEvents.ENTITY_LOAD.register(MobWar::onEntityLoad);
		ServerEntityEvents.ENTITY_LOAD.register(HostBridge::onEntityLoad);
		ServerTickEvents.END_SERVER_TICK.register(WorldBridge::tick);
		LOG.info("passthrough loaded");
	}
}
