package dev.rehan.passthrough.client;

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import com.mojang.blaze3d.platform.InputConstants;
import com.mojang.blaze3d.platform.Window;
import dev.rehan.passthrough.Passthrough;
import dev.rehan.passthrough.client.mixin.KeyMappingAccessor;
import java.util.Locale;
import net.minecraft.client.KeyMapping;
import net.minecraft.client.Minecraft;
import net.minecraft.client.MouseHandler;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.client.input.CharacterEvent;
import net.minecraft.client.input.KeyEvent;
import net.minecraft.client.input.MouseButtonEvent;
import net.minecraft.client.input.MouseButtonInfo;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.world.entity.player.Inventory;
import net.minecraft.core.component.DataComponents;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.component.CustomData;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.EntityHitResult;
import net.minecraft.world.entity.Leashable;
import net.minecraft.world.item.Items;
import dev.rehan.passthrough.Leads;
import dev.rehan.passthrough.MobWar;
import net.minecraft.world.entity.Shearable;
import net.minecraft.world.entity.decoration.LeashFenceKnotEntity;
import net.minecraft.world.phys.HitResult;
import org.lwjgl.sdl.SDLVideo;

/** Host input, applied on the client thread: the host window has the focus, so Minecraft never sees these itself. */
final class ClientInput {
	/** The mouse button held down over a screen (drags), or null. */
	private static MouseButtonInfo held;

	private ClientInput() {
	}

	/** Input the player gives (keys, clicks, the hotbar, the pointer): none of it in the host's cutscenes and scenes. */
	private static final java.util.Set<String> PLAYER_INPUT = java.util.Set.of("key", "slot", "scroll", "cursor", "mbtn", "wheel", "rkey", "chr");

	/** The host's cutscene (loading screen, mission scene) begins: let go of every key and button held. */
	static void releaseAll(final Minecraft minecraft) {
		for (KeyMapping key : new KeyMapping[]{minecraft.options.keyAttack, minecraft.options.keyUse, minecraft.options.keyDrop,
			minecraft.options.keyPickItem, minecraft.options.keySwapOffhand}) {
			key.setDown(false);
		}

		PlayerSync.releaseKeys(minecraft.options);
		if (held != null) {
			minecraft.mouseHandler.onButton(minecraft.getWindow().handle(), held, InputConstants.RELEASE);
			held = null;
		}

	}

	static void handle(final Minecraft minecraft, final JsonObject m) {
		LocalPlayer player = minecraft.player;
		Window window = minecraft.getWindow();
		Screen screen = minecraft.gui.screen();
		String type = m.get("t").getAsString();
		if (!HostState.control() && PLAYER_INPUT.contains(type)
			&& !(type.equals("key") && m.get("k").getAsString().equals("escape")) && !(type.equals("key") && m.has("down") && !m.get("down").getAsBoolean())) {
			return; // (letting go and Escape still count)
		}

		switch (type) {
			case "key" -> {
				String k = m.get("k").getAsString();
				boolean down = !m.has("down") || m.get("down").getAsBoolean();
				if (k.equals("escape")) {
					if (down && screen != null) {
						screen.onClose();
					}

					return;
				}

				KeyMapping key = switch (k) {
					case "use" -> minecraft.options.keyUse;
					case "attack" -> minecraft.options.keyAttack;
					case "pick" -> minecraft.options.keyPickItem;
					case "inventory" -> minecraft.options.keyInventory;
					case "drop" -> minecraft.options.keyDrop;
					case "swap" -> minecraft.options.keySwapOffhand;
					case "chat" -> minecraft.options.keyChat;
					case "command" -> minecraft.options.keyCommand;
					default -> null;
				};
				if (k.equals("attack") && down && player != null && screen == null) {
					swing(minecraft, player);
				}

				if (k.equals("use") && down && player != null && screen == null) {
					leashUse(minecraft, player);
				}

				if (key != null) {
					if (down && !key.isDown()) {
						KeyMappingAccessor access = (KeyMappingAccessor)key;
						access.passthrough$setClickCount(access.passthrough$getClickCount() + 1);
					}

					key.setDown(down);
				}
			}
			case "pset" -> {
				if (player != null) {
					JsonArray at = m.getAsJsonArray("pos");
					PlayerSync.pset(player, m.get("id").getAsInt(), at.get(0).getAsDouble(), at.get(1).getAsDouble(), at.get(2).getAsDouble(),
						m.has("keepY") && m.get("keepY").getAsBoolean());
				}
			}
			case "slot" -> {
				if (player != null) {
					player.getInventory().setSelectedSlot(Math.clamp(m.get("n").getAsInt(), 0, Inventory.getSelectionSize() - 1));
				}
			}
			case "scroll" -> {
				if (player != null) {
					Inventory inventory = player.getInventory();
					int size = Inventory.getSelectionSize();
					inventory.setSelectedSlot(Math.floorMod(inventory.getSelectedSlot() - m.get("d").getAsInt(), size));
				}
			}
			case "hud" -> {
				if (minecraft.gui.hud.isHidden() != m.get("hidden").getAsBoolean()) {
					minecraft.gui.hud.toggle();
				}
			}
			case "view" -> {
				// match the host's picture exactly: un-minimize/un-maximize first (resizing a maximized window is ignored)
				int w = m.get("w").getAsInt(), h = m.get("h").getAsInt();
				long handle = window.handle();
				SDLVideo.SDL_RestoreWindow(handle);
				window.setWindowed(w, h);
				SDLVideo.SDL_SetWindowSize(handle, w, h);
				SDLVideo.SDL_SyncWindow(handle);
			}
			// Minecraft's screens (inventory, chat, ...) worked from the host: its pointer (0..1 of the picture), buttons,
			// wheel and keys, fed in where the window's own input would arrive
			case "cursor" -> {
				MouseHandler mouse = minecraft.mouseHandler;
				double x = m.get("x").getAsDouble() * window.getScreenWidth(), y = m.get("y").getAsDouble() * window.getScreenHeight();
				double dx = x - mouse.xpos(), dy = y - mouse.ypos();
				mouse.onMove(window.handle(), x, y, dx, dy);
				if (screen != null) {
					// (Minecraft only passes moves on to screens while its window has the focus)
					double sx = MouseHandler.getScaledXPos(window, x), sy = MouseHandler.getScaledYPos(window, y);
					screen.mouseMoved(sx, sy);
					if (held != null) {
						screen.mouseDragged(new MouseButtonEvent(sx, sy, held), MouseHandler.getScaledXPos(window, dx), MouseHandler.getScaledYPos(window, dy));
					}

					screen.afterMouseMove();
				}
			}
			case "mbtn" -> {
				MouseButtonInfo button = new MouseButtonInfo(m.get("b").getAsInt(), m.has("m") ? m.get("m").getAsInt() : 0);
				boolean down = m.get("down").getAsBoolean();
				held = down ? button : null;
				if (screen != null) {
					minecraft.mouseHandler.onButton(window.handle(), button, down ? InputConstants.PRESS : InputConstants.RELEASE);
				}
			}
			case "wheel" -> {
				if (screen != null) {
					minecraft.mouseHandler.onScroll(window.handle(), 0.0, m.get("d").getAsDouble());
				}
			}
			case "rkey" -> {
				if (screen != null) {
					// I (the host's inventory key) works as Minecraft's own (E) in screens: it closes the inventory again
					int key = m.get("sc").getAsInt(), code = m.get("kc").getAsInt();
					if (key == InputConstants.KEY_I) {
						key = InputConstants.KEY_E;
						code = 'e';
					}

					// Q is the host's cover key: it never drops what's in the inventory either
					if (key == InputConstants.KEY_Q && screen instanceof net.minecraft.client.gui.screens.inventory.AbstractContainerScreen<?>) {
						return;
					}

					minecraft.keyboardHandler.keyPress(window.handle(), m.get("a").getAsInt(), new KeyEvent(key, code, m.get("m").getAsInt()));
				}
			}
			case "chr" -> {
				if (screen != null) {
					minecraft.keyboardHandler.charTyped(window.handle(), new CharacterEvent(m.get("c").getAsInt()));
				}
			}
			default -> {
			}
		}
	}

	/**
	 * A left click at nothing Minecraft can mine (air, a host person, the host's ground or walls): the host swings the
	 * weapon in hand in its own world. A sword always swings (in creative it never mines).
	 */
	private static void swing(final Minecraft minecraft, final LocalPlayer player) {
		String kind = meleeKind(player.getMainHandItem());
		HitResult hit = minecraft.hitResult;
		boolean mining = hit instanceof BlockHitResult block && hit.getType() == HitResult.Type.BLOCK && minecraft.level != null
			&& !minecraft.level.getBlockState(block.getBlockPos()).is(Blocks.BARRIER) && !minecraft.level.getBlockState(block.getBlockPos()).isAir();
		if (mining && !kind.equals("sword")) {
			return;
		}

		// how charged the swing is (Minecraft's attack cooldown): spam-clicking hits weakly, as in Minecraft; and the
		// weapon's enchantments, which count in the host's world too
		float strength = player.getAttackStrengthScale(0.5F);
		ItemStack weapon = player.getMainHandItem();
		Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"melee\",\"k\":\"%s\",\"s\":%.2f,\"kb\":%d,\"sh\":%d,\"fa\":%d,\"lo\":%d}",
			kind, strength, enchantment(weapon, "knockback"), enchantment(weapon, "sharpness") + enchantment(weapon, "smite") / 2,
			enchantment(weapon, "fire_aspect"), enchantment(weapon, "looting")));
	}

	/**
	 * A right click with a lead or shears at the host's world (not at one of Minecraft's mobs: that's Minecraft's own
	 * lead): the host puts a lead on, ties, takes back or cuts what the crosshair is on. It's told how many of Minecraft's
	 * own mobs are on leads in Steve's hand (they get tied there too).
	 */
	private static void leashUse(final Minecraft minecraft, final LocalPlayer player) {
		ItemStack held = player.getMainHandItem();
		String item = held.is(Items.LEAD) ? "lead" : held.is(Items.SHEARS) ? "shears" : null;
		if (item == null || HostState.frame() == null) {
			return;
		}

		// (only Minecraft's own mobs, knots and sheep are Minecraft's to leash, untie or shear: anything else the crosshair
		// is on, an invisible stand-in of the host's included, is the host's)
		if (minecraft.hitResult instanceof EntityHitResult on) {
			net.minecraft.world.entity.Entity e = on.getEntity();
			if (!Leads.isProxy(e) && !MobWar.isProxy(e) && (e instanceof Leashable || e instanceof LeashFenceKnotEntity
				|| item.equals("shears") && e instanceof Shearable)) {
				return;
			}
		}

		int mobs = 0;
		for (Leashable l : Leashable.leashableLeashedTo(player)) {
			mobs += Leads.isProxy((net.minecraft.world.entity.Entity) l) ? 0 : 1;
		}

		Passthrough.events.accept("{\"t\":\"leashuse\",\"item\":\"" + item + "\",\"mobs\":" + mobs + "}");
	}

	/** The level of an enchantment (by its id's path: "sharpness") on an item, or 0. */
	static int enchantment(final ItemStack stack, final String id) {
		// (over 255, from /enchant: kept in the item's custom data)
		CustomData custom = stack.get(DataComponents.CUSTOM_DATA);
		if (custom != null) {
			int over = custom.copyTag().getCompoundOrEmpty(Passthrough.OVER_LEVELS).getIntOr(id, 0);
			if (over > 0) {
				return over;
			}
		}

		for (var e : stack.getEnchantments().entrySet()) {
			if (e.getKey().unwrapKey().map(k -> k.identifier().getPath().equals(id)).orElse(false)) {
				return e.getIntValue();
			}
		}

		return 0;
	}

	/** fist, tool, sword, axe, trident, spear or mace: how the host lands the swing. */
	private static String meleeKind(final ItemStack stack) {
		if (stack.isEmpty()) {
			return "fist";
		}

		String id = BuiltInRegistries.ITEM.getKey(stack.getItem()).getPath();
		if (id.endsWith("_sword")) {
			return "sword";
		} else if (id.endsWith("_axe")) {
			return "axe";
		} else if (id.endsWith("_spear")) {
			return "spear";
		} else if (id.equals("trident")) {
			return "trident";
		} else if (id.equals("mace")) {
			return "mace";
		} else if (id.endsWith("_pickaxe") || id.endsWith("_shovel") || id.endsWith("_hoe")) {
			return "tool";
		}

		return "fist";
	}
}
