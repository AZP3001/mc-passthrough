package dev.rehan.passthrough;

import java.util.HashMap;
import java.util.Iterator;
import java.util.Map;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.projectile.arrow.AbstractArrow;
import net.minecraft.world.entity.projectile.arrow.ThrownTrident;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.item.enchantment.Enchantment;
import net.minecraft.world.phys.Vec3;

/**
 * Straight (passthrough:straight, a bow's only, one level): its arrows fly dead straight at the speed they left the bow
 * with, no gravity and no slowing in the air, so they go exactly where the crosshair was; gone after 500 blocks if
 * they hit nothing. Server thread.
 */
public final class StraightArrows {
	public static final ResourceKey<Enchantment> STRAIGHT = ResourceKey.create(Registries.ENCHANTMENT, Identifier.fromNamespaceAndPath("passthrough", "straight"));
	private static final double RANGE = 500.0;

	private record Flight(Vec3 from, Vec3 velocity) {
	}

	private static final Map<AbstractArrow, Flight> flying = new HashMap<>();

	private StraightArrows() {
	}

	/** Entity load (spawned): an arrow shot from a bow with Straight flies straight. */
	static void onEntityLoad(final Entity e, final ServerLevel level) {
		if (!(e instanceof AbstractArrow arrow) || e instanceof ThrownTrident) {
			return;
		}

		// a player's arrows leave the bow (or crossbow) 3x as fast, still as fast as it was drawn; as much damage as before
		// (Minecraft's goes with speed). Only when just shot, not when its chunk loads again
		ItemStack bow = arrow.getWeaponItem();
		if (arrow.tickCount == 0 && arrow.getOwner() instanceof net.minecraft.world.entity.player.Player
			&& bow != null && (bow.is(Items.BOW) || bow.is(Items.CROSSBOW))) {
			arrow.setDeltaMovement(arrow.getDeltaMovement().scale(3.0));
			arrow.setBaseDamage(((dev.rehan.passthrough.mixin.AbstractArrowAccessor) arrow).passthrough$baseDamage() / 3.0);
		}

		if (bow == null || !bow.is(Items.BOW) || !has(bow)) {
			return;
		}

		arrow.setNoGravity(true);
		flying.put(arrow, new Flight(arrow.position(), arrow.getDeltaMovement()));
	}

	private static boolean has(final ItemStack stack) {
		return stack.getEnchantments().keySet().stream().anyMatch(holder -> holder.is(STRAIGHT));
	}

	/** Every server tick: in flight at its first speed, along its first line; gone 500 blocks out. */
	static void tick(final MinecraftServer s) {
		if (flying.isEmpty()) {
			return;
		}

		for (Iterator<Map.Entry<AbstractArrow, Flight>> it = flying.entrySet().iterator(); it.hasNext();) {
			Map.Entry<AbstractArrow, Flight> entry = it.next();
			AbstractArrow arrow = entry.getKey();
			Flight flight = entry.getValue();
			if (arrow.isRemoved() || ((dev.rehan.passthrough.mixin.AbstractArrowAccessor) arrow).passthrough$isInGround()) {
				it.remove(); // (it hit something: as any arrow from there)
				continue;
			}

			if (arrow.position().distanceToSqr(flight.from()) > RANGE * RANGE) {
				arrow.discard();
				it.remove();
				continue;
			}

			if (!arrow.isInWater()) {
				arrow.setDeltaMovement(flight.velocity()); // (no air drag either: Minecraft's slowed it to a crawl)
				arrow.needsSync = true;
			}
		}
	}
}
