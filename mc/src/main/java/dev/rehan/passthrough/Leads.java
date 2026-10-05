package dev.rehan.passthrough;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.atomic.AtomicReference;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.sounds.SoundEvent;
import net.minecraft.sounds.SoundEvents;
import net.minecraft.sounds.SoundSource;
import net.minecraft.tags.FluidTags;
import net.minecraft.world.InteractionHand;
import net.minecraft.world.effect.MobEffectInstance;
import net.minecraft.world.effect.MobEffects;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntitySpawnReason;
import net.minecraft.world.entity.EntityTypes;
import net.minecraft.world.entity.Leashable;
import net.minecraft.world.entity.animal.allay.Allay;
import net.minecraft.world.entity.item.ItemEntity;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.entity.vehicle.boat.AbstractBoat;
import net.minecraft.world.entity.vehicle.boat.AbstractChestBoat;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.phys.Vec3;

/**
 * Minecraft's lead on the host's things, and Minecraft's boats carrying the host's people. The host does the pulling and
 * the sitting; Minecraft draws them. Each end of one of the host's leads is a "proxy" (an invisible, still allay) where the
 * host says it is, leashed to Steve (the lead in his hand) or to the proxy at the other end (tied), so Minecraft draws its
 * own lead between them; Minecraft's own mobs can be tied to such a proxy too. Each of the host's people sat in a boat
 * keeps their seat as a proxy riding it. Server thread, but for the queues the link fills.
 */
public final class Leads {
	public static final String TAG = "passthrough_proxy";
	/** Every proxy's entity id: the client (the same game) leaves them where the host puts them, unpicked and unpushed. */
	public static final Set<Integer> PROXY_IDS = ConcurrentHashMap.newKeySet();
	/** For the client: lead id -> {proxy a's entity id, proxy b's} (-1: none). */
	public static volatile Map<Integer, int[]> clientIds = Map.of();

	private record Event(int id, String what, double x, double y, double z) {
	}

	private static final AtomicReference<double[][]> latest = new AtomicReference<>();
	private static final ConcurrentLinkedQueue<Event> events = new ConcurrentLinkedQueue<>();
	private static final ConcurrentLinkedQueue<int[]> grabs = new ConcurrentLinkedQueue<>();
	private static final ConcurrentLinkedQueue<Integer> leaves = new ConcurrentLinkedQueue<>();

	private static final class Lead {
		int kind; // 0 in Steve's hand, 1 tied to b, 2 Minecraft's mobs tied at b
		Allay a, b;
		int age;
		boolean goneSent;
	}

	// server thread
	private static final Map<Integer, Lead> leads = new HashMap<>();
	/** Leads the host no longer lists: gone next tick (this tick's events may still want them). */
	private static final Map<Integer, Lead> dying = new HashMap<>();
	private static final Map<Integer, Allay> seats = new HashMap<>(); // the host's person -> their seat
	private static boolean boatsReported;

	private Leads() {
	}

	public static boolean isProxy(final Entity e) {
		return e != null && PROXY_IDS.contains(e.getId());
	}

	/** {"t":"leashes","l":[[id,kind,ax,ay,az,bx,by,bz],...]} from the host. Any thread. */
	public static void update(final double[][] list) {
		latest.set(list);
	}

	/** {"t":"leashevt","id":n,"e":"new|tied|taken|drop|cut|snap|tiemobs|takemobs","pos":[x,y,z]}. Any thread. */
	public static void event(final int id, final String what, final double x, final double y, final double z) {
		events.add(new Event(id, what, x, y, z));
	}

	/** The host's person `ped` walked into boat `boat` (it has room): a seat for them. Any thread. */
	public static void grab(final int ped, final int boat) {
		grabs.add(new int[] {ped, boat});
	}

	/** The host's person `ped` is out of their boat (died, gone): the seat is free. Any thread. */
	public static void leave(final int ped) {
		leaves.add(ped);
	}

	/** Where a proxy at the lead's end stands so the lead is drawn from exactly (x, y, z) (with no turn: its offset is fixed). */
	public static Vec3 leashedAt(final Entity proxy, final double x, final double y, final double z) {
		Vec3 off = proxy instanceof Leashable l ? l.getLeashOffset() : Vec3.ZERO;
		return new Vec3(x - off.x, y - off.y, z - off.z);
	}

	/** Where a proxy holding a lead stands so it's held at exactly (x, y, z) (Entity.getRopeHoldPosition). */
	public static Vec3 holdingAt(final Entity proxy, final double x, final double y, final double z) {
		return new Vec3(x, y - proxy.getEyeHeight() * 0.7, z);
	}

	/** Put a proxy at `at`, facing nowhere in particular (south: its lead offset is along +z). */
	public static void place(final Entity proxy, final Vec3 at) {
		proxy.setPos(at.x, at.y, at.z);
		proxy.setYRot(0.0F);
		proxy.setXRot(0.0F);
		proxy.setYBodyRot(0.0F);
		proxy.setYHeadRot(0.0F);
		proxy.setDeltaMovement(Vec3.ZERO);
		proxy.noPhysics = true;
	}

	static void tick(final MinecraftServer s) {
		ServerLevel level = s.overworld();
		ServerPlayer player = s.getPlayerList().getPlayers().isEmpty() ? null : s.getPlayerList().getPlayers().get(0);
		dying.values().forEach(Leads::discard);
		dying.clear();
		if (!Passthrough.active) {
			// the host went away: none of it is drawn any more
			if (!leads.isEmpty() || !seats.isEmpty()) {
				clearAll();
			}

			latest.set(null);
			events.clear();
			grabs.clear();
			leaves.clear();
			return;
		}

		double[][] list = latest.getAndSet(null);
		if (list != null) {
			apply(level, player, list);
		}

		for (Lead l : leads.values()) {
			l.age++;
			// held by Steve (he may have respawned: a new player), or tied to its other end
			Entity holder = l.kind == 0 ? player : l.b;
			if (l.a != null && !l.a.isRemoved() && holder != null && l.a.getLeashHolder() != holder) {
				l.a.setLeashedTo(holder, true);
			}
		}

		for (Event e; (e = events.poll()) != null;) {
			handle(level, player, e);
		}

		for (Map.Entry<Integer, Lead> e : leads.entrySet()) {
			Lead l = e.getValue();
			if (l.kind == 2 && !l.goneSent && l.age > 40 && l.b != null && mobsAt(l.b).isEmpty()) {
				// none of Minecraft's mobs is tied there any more: the host forgets the spot
				l.goneSent = true;
				Passthrough.events.accept("{\"t\":\"leashgone\",\"id\":" + e.getKey() + "}");
			}
		}

		boats(level, player);
	}

	private static void apply(final ServerLevel level, final ServerPlayer player, final double[][] list) {
		Set<Integer> seen = new HashSet<>();
		for (double[] r : list) {
			if (r.length < 8) {
				continue;
			}

			int id = (int) r[0];
			seen.add(id);
			Lead l = leads.computeIfAbsent(id, k -> new Lead());
			l.kind = (int) r[1];
			if (l.kind == 2) {
				l.a = discard(l.a);
			} else {
				l.a = ensure(level, l.a, player);
			}

			if (l.kind == 0) {
				l.b = discard(l.b);
			} else {
				l.b = ensure(level, l.b, player);
			}

			if (l.a != null) {
				place(l.a, leashedAt(l.a, r[2], r[3], r[4]));
			}

			if (l.b != null) {
				place(l.b, holdingAt(l.b, r[5], r[6], r[7]));
			}
		}

		leads.entrySet().removeIf(e -> {
			if (seen.contains(e.getKey())) {
				return false;
			}

			dying.put(e.getKey(), e.getValue());
			return true;
		});
		Map<Integer, int[]> ids = new HashMap<>();
		leads.forEach((id, l) -> ids.put(id, new int[] {l.a != null ? l.a.getId() : -1, l.b != null ? l.b.getId() : -1}));
		clientIds = ids;
	}

	private static void handle(final ServerLevel level, final ServerPlayer player, final Event e) {
		Lead l = leads.containsKey(e.id()) ? leads.get(e.id()) : dying.get(e.id()); // (gone from the host's list already: still here this tick)

		boolean creative = player != null && (player.isCreative() || player.isSpectator());
		switch (e.what()) {
			case "new" -> {
				// a lead from Steve's hand on one of the host's things: used up, as on a mob
				if (player != null && !creative) {
					for (InteractionHand hand : InteractionHand.values()) {
						ItemStack held = player.getItemInHand(hand);
						if (held.is(Items.LEAD)) {
							held.shrink(1);
							break;
						}
					}
				}

				sound(level, e, SoundEvents.LEAD_TIED);
			}
			case "tied", "taken" -> sound(level, e, SoundEvents.LEAD_TIED);
			case "tiemobs" -> {
				// Minecraft's mobs on leads in Steve's hand: tied at the host's spot
				if (l != null && l.b != null && player != null) {
					for (Leashable mob : Leashable.leashableLeashedTo(player)) {
						if (!isProxy((Entity) mob)) {
							mob.setLeashedTo(l.b, true);
						}
					}
				}

				sound(level, e, SoundEvents.LEAD_TIED);
			}
			case "takemobs" -> {
				if (l != null && l.b != null && player != null) {
					for (Leashable mob : mobsAt(l.b)) {
						mob.setLeashedTo(player, true);
					}
				}

				sound(level, e, SoundEvents.LEAD_UNTIED);
			}
			case "cut", "drop", "snap" -> {
				if (l != null && l.kind == 2 && l.b != null) {
					mobsAt(l.b).forEach(Leashable::dropLeash); // (each drops its own lead)
				} else if (!creative) {
					ItemEntity item = new ItemEntity(level, e.x(), e.y(), e.z(), new ItemStack(Items.LEAD));
					item.setDefaultPickUpDelay();
					level.addFreshEntity(item);
				}

				sound(level, e, e.what().equals("snap") ? SoundEvents.LEAD_BREAK : e.what().equals("cut") ? SoundEvents.SHEARS_SNIP : SoundEvents.LEAD_UNTIED);
			}
			default -> {
			}
		}
	}

	/** Minecraft's own mobs tied at a proxy. */
	private static List<Leashable> mobsAt(final Entity holder) {
		List<Leashable> out = new ArrayList<>();
		for (Leashable l : Leashable.leashableLeashedTo(holder)) {
			if (!isProxy((Entity) l)) {
				out.add(l);
			}
		}

		return out;
	}

	private static void sound(final ServerLevel level, final Event e, final SoundEvent sound) {
		level.playSound(null, e.x(), e.y(), e.z(), sound, SoundSource.NEUTRAL, 1.0F, 1.0F);
	}

	/** An invisible, still, silent, untouchable allay: one end of a lead or a seat. */
	private static Allay proxy(final ServerLevel level, final Vec3 at) {
		Allay a = EntityTypes.ALLAY.create(level, EntitySpawnReason.COMMAND);
		if (a == null) {
			return null;
		}

		a.setInvisible(true);
		a.addEffect(new MobEffectInstance(MobEffects.INVISIBILITY, MobEffectInstance.INFINITE_DURATION, 0, false, false), null);
		a.setNoAi(true);
		a.setNoGravity(true);
		a.setSilent(true);
		a.setPermanentlyInvulnerable(true);
		a.setPersistenceRequired();
		a.addTag(TAG);
		place(a, at);
		PROXY_IDS.add(a.getId()); // (before it's added: the load event must not take it for one left from an earlier session)
		if (!level.addFreshEntity(a)) {
			PROXY_IDS.remove(a.getId());
			return null;
		}

		return a;
	}

	private static Allay ensure(final ServerLevel level, final Allay a, final ServerPlayer player) {
		if (a != null && !a.isRemoved()) {
			return a;
		}

		discard(a);
		return proxy(level, player != null ? player.position() : Vec3.ZERO);
	}

	private static Allay discard(final Allay a) {
		if (a != null) {
			PROXY_IDS.remove(a.getId());
			if (!a.isRemoved()) {
				a.discard();
			}
		}

		return null;
	}

	private static void discard(final Lead l) {
		l.a = discard(l.a);
		if (l.b != null && !l.b.isRemoved()) {
			mobsAt(l.b).forEach(Leashable::dropLeash); // (Minecraft's own mobs tied there: free, each lead dropped, as at a broken knot)
		}

		l.b = discard(l.b);
	}

	/**
	 * Seats asked for and given up; seats whose boat broke; and {"t":"boats","b":[[id,x,y,z,yaw,vx,vy,vz,free,catches],...],
	 * "s":[[ped,boat,x,y,z,yaw],...]}: the boats near the player (free: seats left; catches: it takes in who walks into it,
	 * as Minecraft's boats do unless a player rows) and where each of the host's people sits, every tick while any.
	 */
	private static void boats(final ServerLevel level, final ServerPlayer player) {
		for (int[] g; (g = grabs.poll()) != null;) {
			if (seats.containsKey(g[0]) || !(level.getEntity(g[1]) instanceof AbstractBoat boat) || !boat.isAlive()
				|| boat.getPassengers().size() >= maxPassengers(boat) || boat.getControllingPassenger() instanceof Player) {
				continue;
			}

			Allay seat = proxy(level, boat.position());
			if (seat == null) {
				continue;
			}

			if (seat.startRiding(boat, true, true)) {
				seats.put(g[0], seat);
			} else {
				discard(seat);
			}
		}

		for (Integer ped; (ped = leaves.poll()) != null;) {
			Allay seat = seats.remove(ped);
			if (seat != null) {
				seat.stopRiding();
				discard(seat);
			}
		}

		// a broken boat lets its passengers go: so does the seat
		seats.values().removeIf(seat -> {
			if (!seat.isRemoved() && seat.getVehicle() instanceof AbstractBoat) {
				return false;
			}

			discard(seat);
			return true;
		});
		if (player == null) {
			return;
		}

		List<AbstractBoat> boats = level.getEntitiesOfClass(AbstractBoat.class, player.getBoundingBox().inflate(64.0), Entity::isAlive);
		if (boats.isEmpty() && seats.isEmpty() && !boatsReported) {
			return;
		}

		StringBuilder b = new StringBuilder("{\"t\":\"boats\",\"b\":[");
		for (AbstractBoat boat : boats) {
			int free = Math.max(0, maxPassengers(boat) - boat.getPassengers().size());
			boolean catches = !(boat.getControllingPassenger() instanceof Player) && !boat.isEyeInFluid(FluidTags.WATER);
			Vec3 v = boat.getDeltaMovement().scale(20.0);
			b.append(b.charAt(b.length() - 1) == '[' ? "" : ",").append(String.format(Locale.ROOT, "[%d,%.3f,%.3f,%.3f,%.1f,%.3f,%.3f,%.3f,%d,%d]",
				boat.getId(), boat.getX(), boat.getY(), boat.getZ(), boat.getYRot(), v.x, v.y, v.z, free, catches ? 1 : 0));
		}

		b.append("],\"s\":[");
		boolean first = true;
		for (Map.Entry<Integer, Allay> e : seats.entrySet()) {
			Allay seat = e.getValue();
			Entity boat = seat.getVehicle();
			if (boat == null) {
				continue;
			}

			b.append(first ? "" : ",").append(String.format(Locale.ROOT, "[%d,%d,%.3f,%.3f,%.3f,%.1f]", e.getKey(), boat.getId(), seat.getX(), seat.getY(),
				seat.getZ(), boat.getYRot()));
			first = false;
		}

		Passthrough.events.accept(b.append("]}").toString());
		boatsReported = !boats.isEmpty() || !seats.isEmpty();
	}

	private static int maxPassengers(final AbstractBoat boat) {
		return boat instanceof AbstractChestBoat ? 1 : 2;
	}

	/** Proxies saved with the world in an earlier session go when they load again. */
	static void onEntityLoad(final Entity e, final ServerLevel level) {
		if (e.entityTags().contains(TAG) && !PROXY_IDS.contains(e.getId())) {
			e.discard();
		}
	}

	private static void clearAll() {
		leads.values().forEach(Leads::discard);
		leads.clear();
		dying.values().forEach(Leads::discard);
		dying.clear();
		for (Allay seat : seats.values()) {
			seat.stopRiding();
			discard(seat);
		}

		seats.clear();
		clientIds = Map.of();
		boatsReported = false;
	}

	/** The world is about to be saved and closed: no proxies in it. */
	static void detach(final MinecraftServer s) {
		clearAll();
		latest.set(null);
		events.clear();
		grabs.clear();
		leaves.clear();
	}
}
