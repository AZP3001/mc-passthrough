package dev.rehan.passthrough.client;

import dev.rehan.passthrough.Passthrough;
import dev.rehan.passthrough.WorldBridge;
import java.util.List;
import java.util.Optional;
import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.fabricmc.fabric.api.networking.v1.ServerPlayConnectionEvents;
import net.minecraft.client.CloudStatus;
import net.minecraft.client.InactivityFpsLimit;
import net.minecraft.client.Minecraft;
import net.minecraft.client.Options;
import net.minecraft.client.gui.screens.ChatScreen;
import net.minecraft.client.gui.screens.DeathScreen;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.client.gui.screens.TitleScreen;
import net.minecraft.client.gui.screens.inventory.AbstractContainerScreen;
import net.minecraft.client.tutorial.TutorialSteps;
import net.minecraft.core.HolderLookup;
import net.minecraft.core.HolderSet;
import net.minecraft.core.component.DataComponents;
import net.minecraft.core.registries.Registries;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.EquipmentSlot;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.item.component.ChargedProjectiles;
import net.minecraft.world.level.GameType;
import net.minecraft.world.level.LevelSettings;
import net.minecraft.world.level.WorldDataConfiguration;
import net.minecraft.world.level.biome.Biomes;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.levelgen.FlatLevelSource;
import net.minecraft.world.level.levelgen.WorldDimensions;
import net.minecraft.world.level.levelgen.WorldOptions;
import net.minecraft.world.level.levelgen.flat.FlatLayerInfo;
import net.minecraft.world.level.levelgen.flat.FlatLevelGeneratorSettings;
import net.minecraft.world.level.levelgen.presets.WorldPresets;

public class PassthroughClient implements ClientModInitializer {
	/** The world the host plays in: an empty (void) creative world, so only what the player builds is Minecraft. */
	private static final String WORLD = "passthrough";
	/** Run on the server once the player has joined. */
	private static final List<String> SETUP = List.of(
		"gamerule advance_time false",
		"gamerule advance_weather false",
		"gamerule spawn_mobs false",
		"gamerule spawn_monsters false",
		"gamerule spawn_patrols false",
		"gamerule spawn_phantoms false",
		"gamerule spawn_wandering_traders false",
		"gamerule send_command_feedback false",
		"gamerule log_admin_commands false",
		"gamerule keep_inventory true",
		// (Minecraft's health is the host's: healing in Minecraft heals the host's player too, so only potions, effects and
		// food count, not Minecraft's own slow regeneration from a full, unseen hunger bar)
		"gamerule natural_health_regeneration false",
		"difficulty normal",
		"gamerule show_advancement_messages false",
		"gamerule player_movement_check false",
		"time set noon",
		"weather clear"
	);
	private static boolean configured;
	private static boolean worldRequested;
	/** The screen kind last told to the host (0 none, 1 chat, 2 inventory/container, 3 other), and when. */
	private static int screenSent = -1;
	private static int screenSentAt;
	/** Whether the player wore an elytra when last told to the host (-1: not told yet), and when. */
	private static int elytraSent = -1;
	private static boolean bowSent;
	private static String effectsSent = "";
	private static int effectsSentAt;
	private static int bowSentAt;
	/** Alt held over one of Minecraft's screens (the host says): the screen and HUD are hidden, the player moves. */
	public static volatile boolean peek;
	private static int elytraSentAt;
	/** Server ticks until the setup commands run (the player isn't in the player list yet when JOIN fires). */
	private static int setupIn = -1;
	private static int respawnIn;

	@Override
	public void onInitializeClient() {
		HostLink.launch();
		ClientTickEvents.END_CLIENT_TICK.register(PassthroughClient::tick);
		ServerPlayConnectionEvents.JOIN.register((handler, sender, server) -> {
			ServerPlayer player = handler.player;
			// never left gliding from a previous session: with no host yet it would glide down into the void
			player.stopFallFlying();
			// (only the elytra a glide put on comes off: the player's own chestplate or elytra stays)
			net.minecraft.world.item.component.CustomData chest = player.getItemBySlot(EquipmentSlot.CHEST).get(DataComponents.CUSTOM_DATA);
			if (chest != null && chest.copyTag().getBooleanOr(dev.rehan.passthrough.WorldBridge.GLIDE_TAG, false)) {
				player.setItemSlot(EquipmentSlot.CHEST, ItemStack.EMPTY);
			}

			// /fly as the player left it (kept with the player)
			Passthrough.flyAllowed = player.entityTags().contains(Passthrough.FLY_TAG);
			if (!player.isCreative() && !player.isSpectator()) {
				player.getAbilities().mayfly = Passthrough.flyAllowed;
			}

			player.getAbilities().flying = player.getAbilities().mayfly;
			player.onUpdateAbilities();
			setupIn = 10;
		});
		ServerTickEvents.END_SERVER_TICK.register(server -> {
			if (setupIn > 0 && --setupIn == 0) {
				SETUP.forEach(WorldBridge::command);
				Passthrough.KIT.forEach(WorldBridge::command); // (only for a player who hasn't had it yet)
			}
		});
	}

	private static void tick(final Minecraft minecraft) {
		// a death (the void) would leave the death screen over the host's picture: respawn straight away
		if (minecraft.player != null && minecraft.gui.screen() instanceof DeathScreen && --respawnIn <= 0) {
			respawnIn = 40;
			minecraft.player.respawn();
		}

		// the host's breath as Minecraft's air bubbles (the server keeps it too: HostBridge)
		if (minecraft.player != null && Passthrough.active && Passthrough.hostAir >= 0.0F && !PlayerSync.walking()) {
			minecraft.player.setAirSupply(Math.round(Passthrough.hostAir * minecraft.player.getMaxAirSupply()));
		}

		if (!configured) {
			configured = true;
			configure(minecraft.options);
		}

		// the host shows its pointer and hands mouse and keyboard over while one of Minecraft's screens is open
		Screen screen = minecraft.gui.screen();
		int kind = minecraft.level == null || screen == null ? 0
			: screen instanceof ChatScreen ? 1 : screen instanceof AbstractContainerScreen<?> ? 2 : 3;
		if (kind != screenSent || (kind != 0 && ++screenSentAt % 20 == 0)) {
			screenSent = kind;
			Passthrough.events.accept("{\"t\":\"screen\",\"kind\":" + kind + "}");
		}

		// the host's own flights (its movement) need an elytra worn, as Minecraft's do; a spectator flies through the
		// host's world as through Minecraft's
		if (minecraft.player != null) {
			int elytra = minecraft.player.getItemBySlot(EquipmentSlot.CHEST).has(DataComponents.GLIDER) ? 1 : 0;
			int armor = minecraft.player.getArmorValue();
			int blocking = minecraft.player.isBlocking() ? 1 : 0;
			int totem = minecraft.player.getMainHandItem().is(Items.TOTEM_OF_UNDYING) || minecraft.player.getOffhandItem().is(Items.TOTEM_OF_UNDYING) ? 1 : 0;
			int scope = minecraft.player.isScoping() ? 1 : 0;
			int state = elytra | (minecraft.player.isSpectator() ? 2 : 0) | (armor << 2) | (blocking << 8) | (totem << 9) | (scope << 10);
			if (state != elytraSent || ++elytraSentAt % 40 == 0) { // (sent at once when it changes: a shield raised counts now)
				elytraSent = state;
				Passthrough.events.accept("{\"t\":\"pstate\",\"ely\":" + elytra + ",\"spec\":" + ((state >> 1) & 1) + ",\"arm\":" + armor + ",\"blk\":" + blocking + ",\"tot\":" + totem + ",\"scope\":" + scope + "}");
			}

			// Minecraft's effects on the player (potions, beacons), felt by the host's player too
			StringBuilder effects = new StringBuilder();
			for (var e : minecraft.player.getActiveEffects()) {
				String id = e.getEffect().unwrapKey().map(k -> k.identifier().getPath()).orElse("");
				if (!id.isEmpty()) {
					effects.append(effects.isEmpty() ? "" : ",").append(id).append(':').append(e.getAmplifier());
				}
			}

			String list = effects.toString();
			if (!list.equals(effectsSent) || ++effectsSentAt % 40 == 0) {
				effectsSent = list;
				Passthrough.events.accept("{\"t\":\"effects\",\"e\":\"" + list + "\"}");
			}

			// a bow drawn, or a loaded crossbow in hand: the host's people it's aimed at are intimidated
			ItemStack using = minecraft.player.getUseItem(), held = minecraft.player.getMainHandItem();
			ChargedProjectiles loaded = held.get(DataComponents.CHARGED_PROJECTILES);
			boolean bow = (minecraft.player.isUsingItem() && (using.is(Items.BOW) || using.is(Items.CROSSBOW)))
				|| (held.is(Items.CROSSBOW) && loaded != null && !loaded.isEmpty());
			if (bow != bowSent || (bow && ++bowSentAt % 20 == 0)) {
				bowSent = bow;
				Passthrough.events.accept("{\"t\":\"bowdraw\",\"on\":" + bow + "}");
			}
		}

		if (!worldRequested && minecraft.level == null && minecraft.gui.screen() instanceof TitleScreen && !Boolean.getBoolean("passthrough.noAutoWorld")) {
			worldRequested = true;
			openWorld(minecraft);
		}
	}

	/** Settings for sitting behind another game: keep running unfocused, and no sky/cloud/bobbing effects in the picture. */
	private static void configure(final Options options) {
		options.pauseOnLostFocus = false;
		options.onboardAccessibility = false;
		options.tutorialStep = TutorialSteps.NONE;
		options.cloudStatus().set(CloudStatus.OFF);
		options.bobView().set(false);
		options.vignette().set(false);
		options.improvedTransparency().set(false);
		options.inactivityFpsLimit().set(InactivityFpsLimit.MINIMIZED);
		options.fovEffectScale().set(0.0);
		options.damageTiltStrength().set(0.0);
		options.menuBackgroundBlurriness().set(0);
		options.enableVsync().set(false);
		// the host shows ~60-120 fps: rendering faster only competes with it for the GPU
		options.framerateLimit().set(120);
		// Minecraft's blocks drawn (and its mobs active) well past 30 blocks around the player
		if (options.renderDistance().get() < 8) {
			options.renderDistance().set(8);
		}

		if (options.simulationDistance().get() < 5) {
			options.simulationDistance().set(5);
		}
		options.save();
	}

	private static void openWorld(final Minecraft minecraft) {
		if (minecraft.getLevelSource().levelExists(WORLD)) {
			Passthrough.LOG.info("opening world {}", WORLD);
			minecraft.createWorldOpenFlows().openWorld(WORLD, () -> minecraft.gui.setScreen(new TitleScreen()));
		} else {
			Passthrough.LOG.info("creating world {}", WORLD);
			LevelSettings settings = new LevelSettings("Passthrough", GameType.CREATIVE, LevelSettings.DifficultySettings.DEFAULT, true, WorldDataConfiguration.DEFAULT);
			minecraft.createWorldOpenFlows().createFreshLevel(WORLD, settings, new WorldOptions(0L, false, false), PassthroughClient::voidWorld, minecraft.gui.screen());
		}
	}

	/** A flat world with a single layer of air: nothing but what gets built (the host's ground arrives as barriers). */
	private static WorldDimensions voidWorld(final HolderLookup.Provider registries) {
		FlatLevelGeneratorSettings flat = new FlatLevelGeneratorSettings(
			Optional.of(HolderSet.direct()), registries.lookupOrThrow(Registries.BIOME).getOrThrow(Biomes.PLAINS), List.of()
		);
		flat.getLayersInfo().add(new FlatLayerInfo(1, Blocks.AIR));
		flat.updateLayers();
		return WorldPresets.createNormalWorldDimensions(registries).replaceOverworldGenerator(registries, new FlatLevelSource(flat));
	}
}
