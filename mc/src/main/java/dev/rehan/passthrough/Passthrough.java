package dev.rehan.passthrough;

import java.util.function.Consumer;
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
