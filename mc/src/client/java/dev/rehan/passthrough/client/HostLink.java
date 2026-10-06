package dev.rehan.passthrough.client;

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import dev.rehan.passthrough.HostBridge;
import dev.rehan.passthrough.HostCollision;
import dev.rehan.passthrough.HostWater;
import dev.rehan.passthrough.Leads;
import dev.rehan.passthrough.MobWar;
import dev.rehan.passthrough.Nether;
import dev.rehan.passthrough.Passthrough;
import dev.rehan.passthrough.TheEnd;
import dev.rehan.passthrough.WorldBridge;
import java.net.InetSocketAddress;
import java.util.Locale;
import net.minecraft.client.Minecraft;
import org.java_websocket.WebSocket;
import org.java_websocket.handshake.ClientHandshake;
import org.java_websocket.server.WebSocketServer;

/**
 * The host's connection: a WebSocket server on 127.0.0.1 (port 25599, or -Dpassthrough.port).
 *
 * <p>Host to Minecraft (JSON, Minecraft coordinates):
 * <ul>
 * <li>{"t":"cam","f":frame,"p":[x,y,z],"r":[yaw,pitch,roll],"fov":vertical degrees,"fp":first person,"pl":[feet x,y,z],"h":body yaw}</li>
 * <li>{"t":"ground","c":[x,z,yBottom,yTop, ...]}: solid columns (barriers)</li>
 * <li>{"t":"hc","b":[x0,y0,z0,x1,y1,z1, ...]}: the host's collision boxes near the player (while walking)</li>
 * <li>{"t":"pset","id":n,"pos":[x,y,z],"keepY":bool}: the host puts the player there (while walking)</li>
 * <li>{"t":"clear"}: remove the barriers placed so far</li>
 * <li>{"t":"cmd","c":"time set noon"}: a server command</li>
 * <li>{"t":"key","k":"use|attack|pick|inventory|drop|swap|escape","down":bool}</li>
 * <li>{"t":"slot","n":0-8}, {"t":"scroll","d":+-1}, {"t":"hud","hidden":bool}, {"t":"view","w":px,"h":px}</li>
 * </ul>
 * Minecraft to host: {"t":"hello",...} on connect, {"t":"explosion","pos":[x,y,z],"r":radius}.
 * Relayed unchanged to the other clients: {"t":"gta",...} (director commands for the host plugin), {"t":"gtastate",...}.
 */
public final class HostLink extends WebSocketServer {
	private static HostLink instance;

	private HostLink(final int port) {
		super(new InetSocketAddress("127.0.0.1", port));
		this.setReuseAddr(true);
		this.setDaemon(true);
	}

	static void launch() {
		int port = Integer.getInteger("passthrough.port", 25599);
		instance = new HostLink(port);
		instance.start();
		Passthrough.events = message -> instance.broadcast(message);
	}

	@Override
	public void onStart() {
		Passthrough.LOG.info("host link listening on 127.0.0.1:{}", this.getPort());
	}

	@Override
	public void onOpen(final WebSocket conn, final ClientHandshake handshake) {
		// a web page open in a browser (browsers always say where the page is from; the host's plugin never does) can't
		// run commands in Minecraft and the host's world
		if (handshake.hasFieldValue("Origin")) {
			Passthrough.LOG.warn("refused a link from a web page ({})", handshake.getFieldValue("Origin"));
			conn.close(1008, "not the host");
			return;
		}

		Passthrough.LOG.info("host connected from {}", conn.getRemoteSocketAddress());
		conn.send(String.format(Locale.ROOT, "{\"t\":\"hello\",\"v\":1,\"shm\":\"%s\",\"pid\":%d}", FrameExporter.NAME.replace("\\", "\\\\"), ProcessHandle.current().pid()));
		// the reach (/range) again: the host's script, restarted, had forgotten it (its hits stopped at 8 blocks)
		conn.send(String.format(Locale.ROOT, "{\"t\":\"gtacmd\",\"c\":\"range\",\"r\":%.2f}", HostBridge.reach));
	}

	@Override
	public void onClose(final WebSocket conn, final int code, final String reason, final boolean remote) {
		Passthrough.LOG.info("host disconnected ({} {})", code, reason);
		WorldBridge.timeScale(1.0F); // (left in the host's slow motion: Minecraft's own speed again)
	}

	@Override
	public void onMessage(final WebSocket conn, final String message) {
		try {
			JsonObject m = JsonParser.parseString(message).getAsJsonObject();
			switch (m.get("t").getAsString()) {
				case "cam" -> HostState.update(m);
				case "hc" -> HostCollision.update(HostState.doubles(m.getAsJsonArray("b")));
				case "ground" -> WorldBridge.solid(ints(m.getAsJsonArray("c")));
				case "clear" -> {
					WorldBridge.clearSolid();
					HostWater.clear();
				}
				case "gwater" -> HostWater.update(ints(m.getAsJsonArray("c")));
				case "restart" -> HostBridge.missionRestart();
				case "timescale" -> WorldBridge.timeScale(m.get("s").getAsFloat());
				case "hitmark" -> HitMarker.hit(m.has("kill") && m.get("kill").getAsBoolean(), m.has("shot") && m.get("shot").getAsBoolean());
				case "xp" -> {
					JsonArray at = m.getAsJsonArray("pos");
					WorldBridge.experience(at.get(0).getAsDouble(), at.get(1).getAsDouble(), at.get(2).getAsDouble(), m.get("n").getAsInt());
				}
				case "waypoint" -> {
					int[] w = m.has("c") ? ints(m.getAsJsonArray("c")) : new int[0];
					WorldBridge.waypoint(w.length >= 3 ? new net.minecraft.core.BlockPos(w[0], w[1], w[2]) : null);
				}
				case "mobkill" -> HostBridge.mobKill(ints(m.getAsJsonArray("ids")));
				case "mobfire" -> HostBridge.mobFire(ints(m.getAsJsonArray("ids")));
				case "gtafire" -> HostBridge.hostFires(ints(m.getAsJsonArray("p")));
				// (the mod's own commands; "chat": as if the player typed it, so it reaches the host too: /kill @e, /time)
				case "cmd" -> WorldBridge.command(m.get("c").getAsString(), m.has("chat") && m.get("chat").getAsBoolean());
				case "gta", "gtastate", "gtainfo", "director" -> this.relay(conn, message);
				case "blocksync" -> WorldBridge.sync(m.has("r") ? m.get("r").getAsInt() : 48);
				case "projhit" -> {
					JsonArray at = m.getAsJsonArray("pos");
					WorldBridge.projectileHit(m.get("id").getAsInt(), at.get(0).getAsDouble(), at.get(1).getAsDouble(), at.get(2).getAsDouble(),
						m.has("stick") && m.get("stick").getAsBoolean());
				}
				case "arrows" -> {
					JsonArray list = m.getAsJsonArray("a");
					double[][] at = new double[list.size()][];
					for (int i = 0; i < at.length; i++) {
						JsonArray e = list.get(i).getAsJsonArray();
						at[i] = new double[] {e.get(0).getAsDouble(), e.get(1).getAsDouble(), e.get(2).getAsDouble(), e.get(3).getAsDouble(),
							e.get(4).getAsDouble(), e.get(5).getAsDouble()};
					}

					int[] gone = m.has("gone") ? ints(m.getAsJsonArray("gone")) : new int[0];
					WorldBridge.stuckArrows(at, gone);
					// the client's copies too, at once (the server's position updates for arrows come only now and then);
					// in one game the two share entity ids
					Minecraft minecraft = Minecraft.getInstance();
					minecraft.execute(() -> {
						if (minecraft.level == null) {
							return;
						}

						for (double[] a : at) {
							net.minecraft.world.entity.Entity e = minecraft.level.getEntity((int) a[0]);
							if (e instanceof net.minecraft.world.entity.projectile.arrow.AbstractArrow) {
								e.setPos(a[1], a[2], a[3]);
								e.setOldPosAndRot();
								e.setYRot((float) a[4]);
								e.setXRot((float) a[5]);
								e.yRotO = e.getYRot();
								e.xRotO = e.getXRot();
							}
						}
					});
				}
				case "peds" -> {
					JsonArray list = m.getAsJsonArray("p");
					double[] flat = new double[list.size() * 4];
					for (int i = 0; i < list.size(); i++) {
						JsonArray e = list.get(i).getAsJsonArray();
						for (int k = 0; k < 4; k++) {
							flat[i * 4 + k] = e.get(k).getAsDouble();
						}
					}

					MobWar.peds(flat);
				}
				case "mobdmg" -> MobWar.damage(m.get("id").getAsInt(), m.get("d").getAsDouble());
				case "spawnmobs" -> MobWar.spawn(m.get("k").getAsString(), m.has("n") ? m.get("n").getAsInt() : 5,
					m.has("rmin") ? m.get("rmin").getAsDouble() : 8.0, m.has("rmax") ? m.get("rmax").getAsDouble() : 16.0,
					m.has("arc") ? m.get("arc").getAsDouble() : 40.0, m.has("yaw") ? m.get("yaw").getAsDouble() : 0.0,
					m.has("at") ? new double[] {m.getAsJsonArray("at").get(0).getAsDouble(), m.getAsJsonArray("at").get(1).getAsDouble(),
						m.getAsJsonArray("at").get(2).getAsDouble()} : null);
				case "mobsclear" -> MobWar.clearMobs();
				case "enderride" -> {
					JsonArray at = m.has("pos") ? m.getAsJsonArray("pos") : null;
					TheEnd.ride(m.get("id").getAsInt(), at == null ? null : new double[] {at.get(0).getAsDouble(), at.get(1).getAsDouble(),
						at.get(2).getAsDouble(), m.has("yaw") ? m.get("yaw").getAsDouble() : 0.0});
				}
				case "totem" -> WorldBridge.totem();
				case "leashes" -> {
					JsonArray list = m.getAsJsonArray("l");
					double[][] leads = new double[list.size()][];
					for (int i = 0; i < leads.length; i++) {
						leads[i] = HostState.doubles(list.get(i).getAsJsonArray());
					}

					Leads.update(leads);
					// the client's copies of the ends at once, every frame (the server's come a tick or two later)
					Minecraft minecraft = Minecraft.getInstance();
					minecraft.execute(() -> placeLeadEnds(minecraft, leads));
				}
				case "leashevt" -> {
					JsonArray at = m.getAsJsonArray("pos");
					Leads.event(m.get("id").getAsInt(), m.get("e").getAsString(), at.get(0).getAsDouble(), at.get(1).getAsDouble(), at.get(2).getAsDouble());
				}
				case "boatgrab" -> Leads.grab(m.get("ped").getAsInt(), m.get("boat").getAsInt());
				case "boatleave" -> Leads.leave(m.get("ped").getAsInt());
				case "hostshot" -> {
					JsonArray at = m.getAsJsonArray("pos"), dir = m.getAsJsonArray("dir");
					WorldBridge.hostShot(at.get(0).getAsDouble(), at.get(1).getAsDouble(), at.get(2).getAsDouble(), dir.get(0).getAsDouble(),
						dir.get(1).getAsDouble(), dir.get(2).getAsDouble(), m.has("boom") && m.get("boom").getAsBoolean());
				}
				case "peek" -> PassthroughClient.peek = m.has("on") && m.get("on").getAsBoolean();
				case "portal" -> {
					JsonArray at = m.getAsJsonArray("at");
					Nether.buildPortal(at.get(0).getAsDouble(), at.get(1).getAsDouble(), at.get(2).getAsDouble(), m.get("yaw").getAsFloat());
				}
				case "netheroff" -> Nether.stop();
				case "nethersync" -> {
					Nether.resync();
					TheEnd.resync();
				}
				case "glide" -> WorldBridge.glide(!m.has("on") || m.get("on").getAsBoolean(), m.has("speed") ? m.get("speed").getAsDouble() : 1.2,
					!m.has("equip") || m.get("equip").getAsBoolean());
				default -> {
					Minecraft minecraft = Minecraft.getInstance();
					minecraft.execute(() -> ClientInput.handle(minecraft, m));
				}
			}
		} catch (RuntimeException e) {
			Passthrough.LOG.warn("bad host message {}: {}", message.length() > 200 ? message.substring(0, 200) : message, e.toString());
		}
	}

	/** The ends of the host's leads where the host says they are now (client thread). */
	private static void placeLeadEnds(final Minecraft minecraft, final double[][] leads) {
		if (minecraft.level == null) {
			return;
		}

		java.util.Map<Integer, int[]> ids = Leads.clientIds;
		for (double[] r : leads) {
			int[] proxy = r.length >= 8 ? ids.get((int) r[0]) : null;
			if (proxy == null) {
				continue;
			}

			net.minecraft.world.entity.Entity a = proxy[0] >= 0 ? minecraft.level.getEntity(proxy[0]) : null;
			net.minecraft.world.entity.Entity b = proxy[1] >= 0 ? minecraft.level.getEntity(proxy[1]) : null;
			if (a != null) {
				Leads.place(a, Leads.leashedAt(a, r[2], r[3], r[4]));
				a.setOldPosAndRot();
			}

			if (b != null) {
				Leads.place(b, Leads.holdingAt(b, r[5], r[6], r[7]));
				b.setOldPosAndRot();
			}
		}
	}

	/** Messages between the host plugin and a director script pass through Minecraft's link to every other client. */
	private void relay(final WebSocket from, final String message) {
		for (WebSocket c : this.getConnections()) {
			if (c != from && c.isOpen()) {
				c.send(message);
			}
		}
	}

	@Override
	public void onError(final WebSocket conn, final Exception e) {
		Passthrough.LOG.warn("host link error", e);
	}

	private static int[] ints(final JsonArray a) {
		int[] out = new int[a.size()];
		for (int i = 0; i < out.length; i++) {
			out[i] = a.get(i).getAsInt();
		}

		return out;
	}
}
