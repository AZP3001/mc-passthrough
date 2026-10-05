// Composites Minecraft (passthrough mod) into GTA V. The MCPassthrough add-on (MCPassthrough.asi) uploads
// Minecraft's latest frame into the MCWORLD / MCDEPTH / MCOVERLAY textures and sets the plane uniforms.
//
// world: premultiplied RGBA, drawn where Minecraft is nearer than GTA's depth buffer (both in metres).
// overlay: Minecraft's hand, HUD and screens, always on top.
#include "ReShade.fxh"

texture MCWorldTex : MCWORLD;
texture MCDepthTex : MCDEPTH;
texture MCOverlayTex : MCOVERLAY;
// The cells mined out of GTA's world round the camera (64 x 64 x 64 as 64 slices of 64 x 64: slice y at column
// (y % 8) * 64, row (y / 8) * 64): 1 dug out, about 0.5 one of the blocks Minecraft fills round a hole with.
texture MCDigTex : MCDIG;
sampler sWorld { Texture = MCWorldTex; AddressU = CLAMP; AddressV = CLAMP; };
sampler sDepth { Texture = MCDepthTex; MinFilter = POINT; MagFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };
sampler sOverlay { Texture = MCOverlayTex; AddressU = CLAMP; AddressV = CLAMP; };
sampler sDig { Texture = MCDigTex; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };

// x = near, y = far, z = flags (1: [0,1] depth, 2: rows bottom-up, 4: reversed Z). Set by the add-on.
uniform float3 McPlanes = float3(0.05, 2048.0, 7.0);
// GTA's camera near/far clip. Set by the add-on.
uniform float2 HostPlanes = float2(0.15, 10000.0);

// Set true by the add-on only while it has uploaded a Minecraft frame; until then GTA passes through untouched.
uniform bool McActive = false;

uniform bool HostReversedZ < ui_label = "GTA depth is reversed"; > = true;
uniform float DepthBias < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.005; ui_label = "Depth bias (m)";
	ui_tooltip = "How far behind GTA's surface Minecraft may still show (blocks resting on the ground)."; > = 0.04;
uniform float SlopeBias < ui_type = "drag"; ui_min = 0.0; ui_max = 40.0; ui_step = 0.1; ui_label = "Depth bias per grazing slope";
	ui_tooltip = "Extra depth bias where GTA's surface is seen at a grazing angle (its depth changes fast down the screen): Minecraft ground laid into GTA's (lava, netherrack) still shows far away."; > = 0.0;
uniform float MaxBias < ui_type = "drag"; ui_min = 0.1; ui_max = 10.0; ui_step = 0.1; ui_label = "Largest depth bias (m)"; > = 3.0;
uniform float HostDepthScale < ui_type = "drag"; ui_min = 0.5; ui_max = 2.0; ui_step = 0.001; ui_label = "GTA depth scale";
	ui_tooltip = "Calibration: multiplies GTA's linearised depth."; > = 1.0;
uniform float LightMatch < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Match GTA lighting";
	ui_tooltip = "Relight Minecraft by GTA's surfaces around it (not its sky): darker in shade and at night, tinted by nearby light."; > = 0.85;
uniform float LightReference < ui_type = "drag"; ui_min = 0.1; ui_max = 1.0; ui_step = 0.01; ui_label = "Neutral brightness";
	ui_tooltip = "GTA brightness at which Minecraft keeps its own colours."; > = 0.40;
uniform float MinLight < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Darkest light";
	ui_tooltip = "How dark Minecraft may get in GTA's darkest places (Minecraft renders in full daylight)."; > = 0.10;
uniform float MaxLight < ui_type = "drag"; ui_min = 0.5; ui_max = 2.0; ui_step = 0.01; ui_label = "Brightest light";
	ui_tooltip = "How much brighter than its own colours Minecraft may get in GTA's brightest places. Over 1 it glows."; > = 1.0;
uniform float LightTint < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Light colour"; > = 0.2;
uniform float LightBlur < ui_type = "drag"; ui_min = 1.0; ui_max = 6.0; ui_step = 0.1; ui_label = "Light blur (mip)"; > = 3.5;
uniform float McGamma < ui_type = "drag"; ui_min = 0.5; ui_max = 2.0; ui_step = 0.01; ui_label = "Minecraft gamma";
	ui_tooltip = "Over 1 darkens Minecraft's mid tones toward GTA's (Minecraft's textures are bright and flat)."; > = 1.12;
uniform float Saturation < ui_type = "drag"; ui_min = 0.0; ui_max = 1.5; ui_step = 0.01; ui_label = "Minecraft saturation"; > = 0.88;
uniform float SaturationMatch < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Match GTA saturation";
	ui_tooltip = "Make Minecraft as colourful as GTA's world around it: greyer on a grey street, as vivid in a park."; > = 0.25;
uniform float SolidAlpha < ui_type = "drag"; ui_min = 0.3; ui_max = 1.0; ui_step = 0.01; ui_label = "Solid from coverage";
	ui_tooltip = "Minecraft pixels at least this covered are drawn fully solid (only water, glass, ice and edges blend with GTA)."; > = 0.45;
uniform float GradeMatch < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Match GTA colour grade";
	ui_tooltip = "Give Minecraft the colour of GTA's whole picture (a red Nether night, an orange sunset), as GTA's own grading does to its world."; > = 0.3;
uniform float HazeStart < ui_type = "drag"; ui_min = 0.0; ui_max = 200.0; ui_step = 1.0; ui_label = "Haze start (m)"; > = 90.0;
uniform float HazeDistance < ui_type = "drag"; ui_min = 10.0; ui_max = 1000.0; ui_step = 1.0; ui_label = "Haze distance (m)";
	ui_tooltip = "Far away, Minecraft fades toward GTA's overall colour (GTA's haze and fog don't reach Minecraft otherwise)."; > = 350.0;
uniform float HazeStrength < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Haze strength"; > = 0.35;
uniform float EdgeSoftness < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Soften Minecraft's edges";
	ui_tooltip = "Anti-alias where Minecraft meets GTA (GTA's own anti-aliasing ran before Minecraft was added)."; > = 0.45;
uniform float ContactShadow < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Contact shadows";
	ui_tooltip = "Darken GTA's surfaces right next to Minecraft's (under mobs' feet, around blocks)."; > = 0.5;
uniform float ContactRadius < ui_type = "drag"; ui_min = 0.1; ui_max = 3.0; ui_step = 0.05; ui_label = "Contact shadow size (m)"; > = 0.7;
uniform float BloomStrength < ui_type = "drag"; ui_min = 0.0; ui_max = 2.0; ui_step = 0.01; ui_label = "Minecraft glow";
	ui_tooltip = "Bright Minecraft things (lava, fire, the portal) glow over GTA's picture, as GTA's own lights do."; > = 0.0;
uniform float BloomThreshold < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Glow threshold";
	ui_tooltip = "Only Minecraft things brighter than this glow (lava, fire, glowstone; not ordinary blocks in daylight)."; > = 0.97;
// Set by the add-on: the camera shake, applied to the finished picture so GTA and Minecraft shake together
// (x, y: fraction of the screen height; z: roll, radians), and the nether portal's warp (0..1) while passing through.
uniform float3 Shake = float3(0.0, 0.0, 0.0);
uniform float PortalWarp = 0.0;
// Set by the add-on: GTA's minimap (x0, y0, x1, y1 in 0..1), which Minecraft's world never covers; x1 <= x0 for none.
uniform float4 HudMask = float4(0.0, 0.0, 0.0, 0.0);
// Set by the add-on: the mouse pointer over Minecraft's screens (x, y in 0..1, z > 0.5 shown). GTA draws its own
// pointer, but under Minecraft's overlay, so this one goes on top.
uniform float3 McCursor = float3(0.0, 0.0, 0.0);
uniform float Timer < source = "timer"; >;
uniform int DebugView < ui_type = "combo"; ui_items = "Composite\0GTA depth (1 m bands)\0Minecraft depth (1 m bands)\0Depth difference\0"; > = 0;
uniform bool Reproject < ui_label = "Re-project to GTA's camera"; ui_tooltip = "Rotate Minecraft's (slightly older) frame onto GTA's current camera."; > = true;
uniform float PosePrediction < ui_type = "drag"; ui_min = -2.0; ui_max = 3.0; ui_step = 0.05; ui_label = "Pose prediction (frames)";
	ui_tooltip = "Extrapolate GTA's camera rotation by this many frames before re-projecting."; > = 0.0;

// Set by the add-on: rows of R_mc^T R_gta (GTA camera ray -> Minecraft camera ray), and
// x = tan(GTA vertical fov / 2), y = tan(Minecraft vertical fov / 2), z = Minecraft aspect.
uniform float3 WarpRow0 = float3(1.0, 0.0, 0.0);
uniform float3 WarpRow1 = float3(0.0, 1.0, 0.0);
uniform float3 WarpRow2 = float3(0.0, 0.0, 1.0);
uniform float3 WarpTan = float3(0.7, 0.7, 1.7777);
// Set by the add-on: GTA's camera position relative to Minecraft's, in Minecraft's camera space.
uniform float3 WarpT = float3(0.0, 0.0, 0.0);
// Set by the add-on: the same for Steve (the camera's motion less his own), his screen box (x0, y0, x1, y1; x1 <= x0
// none), and his depth range and the glass in front of him (near, far, glass distance or 0), in metres.
uniform float3 SteveT = float3(0.0, 0.0, 0.0);
uniform float4 SteveBox = float4(0.0, 0.0, 0.0, 0.0);
uniform float3 SteveDepth = float3(0.0, 0.0, 0.0);
// Set by the add-on: Steve's feet where Minecraft drew him, in the camera it drew him with (xyz; w: his height, 0 none), and
// world up in that camera (xyz; w: 1 when he sits in a car).
uniform float4 SteveMc = float4(0.0, 0.0, 0.0, 0.0);
uniform float4 SteveUp = float4(0.0, 1.0, 0.0, 0.0);
uniform float SteveBias < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Steve's depth allowance (m)";
	ui_tooltip = "How far behind GTA's surfaces Steve may still show (he's wider than GTA's character)."; > = 0.2;
// Set by the add-on while cells are mined out of GTA's world: GTA's camera and Minecraft's (position from the cells'
// corner, and the rows of the camera-to-world rotation).
uniform float DigOn = 0.0;
uniform float3 DigHostPos = float3(0.0, 0.0, 0.0);
uniform float3 DigHostRow0 = float3(1.0, 0.0, 0.0);
uniform float3 DigHostRow1 = float3(0.0, 1.0, 0.0);
uniform float3 DigHostRow2 = float3(0.0, 0.0, 1.0);
uniform float3 DigMcPos = float3(0.0, 0.0, 0.0);
uniform float3 DigMcRow0 = float3(1.0, 0.0, 0.0);
uniform float3 DigMcRow1 = float3(0.0, 1.0, 0.0);
uniform float3 DigMcRow2 = float3(0.0, 0.0, 1.0);
uniform float MarchThickness < ui_type = "drag"; ui_min = 0.05; ui_max = 3.0; ui_step = 0.05; ui_label = "Re-projection thickness (m)";
	ui_tooltip = "How deep Minecraft's surfaces count when re-projecting: smaller stops thin things smearing on camera moves."; > = 0.6;

// GTA's picture at quarter size with mips: its blurred levels stand in for the light around each pixel. GtaLightTex
// holds GTA's surfaces only (colour x w, w; w = 0 for the sky), so a block against the sky isn't lit by the sky (that made
// Minecraft glow); GtaStatTex holds the whole picture (colour) and how colourful its surfaces are (saturation x w).
texture GtaLightTex { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA16F; MipLevels = 9; };
sampler sGtaLight { Texture = GtaLightTex; AddressU = CLAMP; AddressV = CLAMP; };
texture GtaStatTex { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA16F; MipLevels = 9; };
sampler sGtaStat { Texture = GtaStatTex; AddressU = CLAMP; AddressV = CLAMP; };

// The composited world (GTA + Minecraft, no overlay) and, per pixel: Minecraft's coverage, its depth and GTA's.
texture CompositeTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
sampler sComposite { Texture = CompositeTex; AddressU = CLAMP; AddressV = CLAMP; };
texture McInfoTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA16F; };
sampler sMcInfo { Texture = McInfoTex; MinFilter = POINT; MagFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };
// Minecraft's bright parts at quarter size with mips (the glow).
texture McBrightTex { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA16F; MipLevels = 6; };
sampler sMcBright { Texture = McBrightTex; AddressU = CLAMP; AddressV = CLAMP; };

float luma(float3 c)
{
	return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float mc_linear(float d)
{
	const float n = McPlanes.x, f = McPlanes.y;
	if (d <= 0.0)
		return 1e9;
	return n * f / (n + d * (f - n));
}

float host_linear(float d)
{
	const float n = HostPlanes.x, f = HostPlanes.y;
	float z;
	if (HostReversedZ)
		z = d > 0.0 ? n * f / (n + d * (f - n)) : 1e9;
	else
		z = d < 1.0 ? n * f / (f - d * (f - n)) : 1e9;
	return z * HostDepthScale;
}

/// GTA's picture for the light estimate: its surfaces (colour, weight 1) and its sky (weight 0), and how colourful each
/// surface is. (Depth past 1.5 km is sky.)
void PS_Light(float4 pos : SV_Position, float2 uv : TEXCOORD, out float4 light : SV_Target0, out float4 stat : SV_Target1)
{
	const float3 c = tex2D(ReShade::BackBuffer, uv).rgb;
	const float w = host_linear(tex2Dlod(ReShade::DepthBuffer, float4(uv, 0, 0)).x) < 1500.0 ? 1.0 : 0.0;
	const float hi = max(c.r, max(c.g, c.b)), lo = min(c.r, min(c.g, c.b));
	light = float4(c * w, w);
	stat = float4(c, (hi > 1e-3 ? (hi - lo) / hi : 0.0) * w);
}

/// The light of GTA's surfaces around uv (colour, weight; the sky left out), at blur `mip`, or wider where there's little.
float4 surface_light(float2 uv, float mip, out float sat)
{
	float4 L = tex2Dlod(sGtaLight, float4(uv, 0, mip));
	float4 S = tex2Dlod(sGtaStat, float4(uv, 0, mip));
	if (L.a < 0.1)
	{
		L = tex2Dlod(sGtaLight, float4(uv, 0, mip + 2.0));
		S = tex2Dlod(sGtaStat, float4(uv, 0, mip + 2.0));
	}
	if (L.a < 0.02)
	{
		L = tex2Dlod(sGtaLight, float4(0.5, 0.5, 0, 8.0));
		S = tex2Dlod(sGtaStat, float4(0.5, 0.5, 0, 8.0));
	}
	sat = S.a / max(L.a, 1e-3);
	return float4(L.rgb / max(L.a, 1e-3), L.a);
}

/// Minecraft renders in full daylight with bright, vivid textures: treat its colour as albedo, bring its gamma and
/// saturation to GTA's world around it, light it with GTA's surfaces around it (never brighter than its own colours:
/// that glowed), give it GTA's colour grade (the whole picture's colour) and, far away, GTA's haze, toward GTA's overall
/// colour (never toward what's behind it: that looked see-through).
float3 relight(float3 c, float2 uv, float zm)
{
	float sat;
	const float3 L = surface_light(uv, LightBlur, sat).rgb;
	const float lum = luma(L);
	c = pow(max(c, 0.0), McGamma);
	const float satMatch = lerp(1.0, clamp(sat / 0.3, 0.55, 1.15), SaturationMatch);
	c = max(lerp(luma(c).xxx, c, Saturation * satMatch), 0.0);
	const float gain = clamp(lum / LightReference, MinLight, MaxLight);
	const float3 tint = clamp(lerp(1.0, L / max(lum, 1e-3), LightTint), 0.7, 1.3);
	c = lerp(c, c * gain * tint, LightMatch);
	// the picture's grade, gentled (GTA's own grade keeps some colour in everything) and kept off lights (bright
	// things keep their colour: fire, lava, the portal)
	const float3 M = tex2Dlod(sGtaStat, float4(0.5, 0.5, 0, 8.0)).rgb;
	const float3 grade = clamp(lerp(1.0, M / max(luma(M), 1e-3), 0.6), 0.6, 1.6);
	c = lerp(c, c * grade, GradeMatch * (1.0 - saturate((max(c.r, max(c.g, c.b)) - 0.45) * 2.5)));
	const float h = saturate((1.0 - exp(-max(zm - HazeStart, 0.0) / HazeDistance)) * HazeStrength);
	return lerp(c, M, h);
}

/// The cell at p (from the dug cells' corner): 1 dug out, about 0.5 a block filling round a hole, 0 neither.
float dig_cell(float3 p)
{
	const float3 c = floor(p);
	if (any(c < 0.0) || any(c > 63.0))
		return 0.0;
	const float row = floor(c.y / 8.0);
	const float2 texel = float2((c.y - row * 8.0) * 64.0 + c.x, row * 64.0 + c.z);
	return tex2Dlod(sDig, float4((texel + 0.5) / 512.0, 0, 0)).r;
}

/// Whether a GTA surface point (GTA's camera space) is in a cell dug out (or on its edge, just in front of it along the
/// view: the street over a hole dug under it).
bool dug_host(float3 pc)
{
	const float3 p = DigHostPos + float3(dot(DigHostRow0, pc), dot(DigHostRow1, pc), dot(DigHostRow2, pc));
	const float3 d = normalize(float3(dot(DigHostRow0, pc), dot(DigHostRow1, pc), dot(DigHostRow2, pc)));
	return dig_cell(p) > 0.75 || dig_cell(p + d * 0.04) > 0.75;
}

/// Whether a point of Minecraft's picture (its camera's space) is on the outside of a block filling round a hole (a face
/// not on the hole: GTA's own wall or street is there, and the block must not show through it or stick out of it).
bool filler_outside(float3 qm)
{
	const float3 p = DigMcPos + float3(dot(DigMcRow0, qm), dot(DigMcRow1, qm), dot(DigMcRow2, qm));
	float fill = 0.0, dug = 0.0;
	[unroll] for (int a = 0; a < 3; ++a)
	{
		float3 o = 0.0;
		o[a] = 0.03;
		const float v0 = dig_cell(p + o), v1 = dig_cell(p - o);
		fill = max(fill, max(v0 > 0.25 && v0 < 0.75 ? 1.0 : 0.0, v1 > 0.25 && v1 < 0.75 ? 1.0 : 0.0));
		dug = max(dug, max(v0 > 0.75 ? 1.0 : 0.0, v1 > 0.75 ? 1.0 : 0.0));
	}
	return fill > 0.5 && dug < 0.5;
}

float3 bands(float z)
{
	const float t = frac(z);
	return lerp(float3(0.1, 0.1, 0.1), float3(1.0, 0.85, 0.3), step(0.5, t)) * saturate(1.5 - z / 100.0);
}

/// Where Minecraft's frame has `p` (GTA camera space) in it: its uv's ndc and how far behind Minecraft's surface there the
/// point is (negative: in front). False off the frame.
bool mc_sample(float3 p, float3 T, out float2 n, out float behind)
{
	const float2 mcScale = float2(WarpTan.y * WarpTan.z, WarpTan.y);
	const float3 pm = float3(dot(WarpRow0, p), dot(WarpRow1, p), dot(WarpRow2, p)) + T;
	n = 0.0;
	behind = -1e9;
	if (pm.z > -1e-3)
		return false;
	n = pm.xy / -pm.z / mcScale;
	if (any(abs(n) > 1.0))
		return false;
	const float zmv = mc_linear(tex2Dlod(sDepth, float4(n * 0.5 + 0.5, 0, 0)).r);
	behind = -pm.z - zmv;
	return zmv < 1e8;
}

/// March a GTA pixel's ray (GTA's current camera) from near to far, moved into the camera Minecraft rendered with
/// (rotation, and translation T): where it first goes behind Minecraft's surface, Minecraft is seen. The crossing is
/// found exactly (bisection) and must be a real one: a ray that only gets behind a thin thing (Steve's edge, a post)
/// far beyond it sees past it (taking that as a hit smeared thin things across the screen as the camera moved).
bool march(float3 ray, float3 T, float zFar, out float2 muv, out float zm, out float3 qm)
{
	const float2 mcScale = float2(WarpTan.y * WarpTan.z, WarpTan.y);
	const float zNear = 0.2;
	muv = 0.0;
	zm = 1e9;
	qm = 0.0;
	float prev = zNear;
	[loop] for (int i = 0; i < 32; ++i)
	{
		const float z = zNear * pow(zFar / zNear, i / 31.0);
		float2 n;
		float behind;
		if (!mc_sample(ray * z, T, n, behind) || behind < -0.03)
		{
			prev = z;
			continue;
		}
		// the crossing between the last point in front and this one
		float lo = prev, hi = z;
		[loop] for (int b = 0; b < 6; ++b)
		{
			const float mid = 0.5 * (lo + hi);
			float2 nm;
			float bm;
			if (mc_sample(ray * mid, T, nm, bm) && bm >= -0.03)
				hi = mid;
			else
				lo = mid;
		}
		mc_sample(ray * hi, T, n, behind);
		if (behind > MarchThickness)
		{
			prev = z;
			continue; // behind a thin thing, not on it
		}
		// settle on the surface (fixed point on the surface point's GTA depth)
		float zg = hi;
		float2 ndc = n;
		{
			const float3 ph = ray * hi;
			qm = float3(dot(WarpRow0, ph), dot(WarpRow1, ph), dot(WarpRow2, ph)) + T;
		}
		[loop] for (int k = 0; k < 3; ++k)
		{
			const float3 pk = ray * zg;
			const float3 mk = float3(dot(WarpRow0, pk), dot(WarpRow1, pk), dot(WarpRow2, pk)) + T;
			if (mk.z > -1e-3)
				break;
			const float2 nk = mk.xy / -mk.z / mcScale;
			if (any(abs(nk) > 1.0))
				break;
			const float zk = mc_linear(tex2Dlod(sDepth, float4(nk * 0.5 + 0.5, 0, 0)).r);
			if (zk > 1e8)
				break;
			ndc = nk;
			qm = float3(nk * mcScale, -1.0) * zk; // (the point found, in Minecraft's camera)
			const float3 q = qm - T;
			zg = -(q.x * WarpRow0.z + q.y * WarpRow1.z + q.z * WarpRow2.z);
		}
		muv = ndc * 0.5 + 0.5;
		zm = zg;
		return true;
	}
	return false;
}

/// How high over Steve's feet a point of Minecraft's frame (its camera's space) is, or -1 if it isn't on Steve (outside his
/// body's column: a little wider than him).
float on_steve(float3 qm)
{
	if (SteveMc.w <= 0.0)
		return -1.0;
	const float3 d = qm - SteveMc.xyz;
	const float h = dot(d, SteveUp.xyz);
	const float3 side = d - SteveUp.xyz * h;
	return h > -0.25 && h < SteveMc.w + 0.35 && dot(side, side) < 0.6 * 0.6 ? max(h, 0.0) : -1.0;
}

void PS_Composite(float4 pos : SV_Position, float2 uv : TEXCOORD, out float4 outColor : SV_Target0, out float4 outInfo : SV_Target1)
{
	const float3 host = tex2D(ReShade::BackBuffer, uv).rgb;
	outInfo = 0.0;
	outColor = float4(host, 1.0);
	if (!McActive)
		return;
	// GTA's minimap stays visible: Minecraft's world never covers it
	if (HudMask.z > HudMask.x && uv.x >= HudMask.x && uv.x <= HudMask.z && uv.y >= HudMask.y && uv.y <= HudMask.w)
		return;
	const float2 ouv = float2(uv.x, 1.0 - uv.y); // Minecraft's rows are bottom-up

	// Where this GTA pixel's view ray lands in Minecraft's frame.
	float2 muv = ouv;
	bool inside = true;
	const float zhSeen = host_linear(tex2Dlod(ReShade::DepthBuffer, float4(uv, 0, 0)).x);
	const float3 ray = float3((uv.x * 2.0 - 1.0) * WarpTan.x * BUFFER_WIDTH * BUFFER_RCP_HEIGHT, (1.0 - uv.y * 2.0) * WarpTan.x, -1.0);
	// how far behind GTA's surface Minecraft may still show: more where that surface is seen at a grazing angle
	const float allow = min(DepthBias + SlopeBias * abs(ddy(zhSeen)), max(MaxBias, DepthBias));
	// GTA's wall, street or ground mined out here: it isn't there (whatever Minecraft has behind it shows: the hole's sides)
	const float zh = DigOn > 0.5 && zhSeen < 2000.0 && dug_host(ray * zhSeen) ? 1e5 : zhSeen;
	float zm = 1e9;
	float steveH = -1.0; // how high on Steve this pixel is (-1: not Steve)
	if (Reproject)
	{
		const float zFar = max(min(zh + allow + SteveBias + 0.5, 400.0), 0.5);
		inside = false; // (found by the marches below, or not at all)
		// Steve moves with the camera (it follows him, a car carries both): his part of the picture moves by the camera's
		// motion less his own, and only counts where it lands on him
		const bool inBox = SteveBox.z > SteveBox.x && uv.x >= SteveBox.x && uv.x <= SteveBox.z && uv.y >= SteveBox.y && uv.y <= SteveBox.w;
		float3 qm;
		if (inBox)
		{
			float2 sm;
			float sz;
			if (march(ray, SteveT, zFar + 1.5, sm, sz, qm) && sz > SteveDepth.x - 0.4 && sz < SteveDepth.y + 0.4)
			{
				steveH = on_steve(qm);
				if (steveH >= 0.0)
				{
					muv = sm;
					zm = sz;
					inside = true;
				}
			}
		}
		if (!inside)
		{
			inside = march(ray, WarpT, zFar, muv, zm, qm);
			// Steve's old place (where Minecraft drew him) moved by the camera alone: not him (he's drawn above where he
			// is now). It showed him twice.
			if (inside && on_steve(qm) >= 0.0)
			{
				inside = false;
				zm = 1e9;
			}
		}
		// the outside of a block filling round a hole: GTA's surface shows there, not the block
		if (inside && DigOn > 0.5 && filler_outside(qm))
		{
			inside = false;
			zm = 1e9;
		}
	}
	float4 world = inside ? tex2D(sWorld, muv) : 0.0;
	// Minecraft's blocks and mobs are solid: only its own see-through things (water, glass, ice) and the edges blend
	if (world.a >= SolidAlpha)
		world = float4(world.rgb / world.a, 1.0);
	if (!Reproject)
		zm = mc_linear(tex2D(sDepth, muv).r);

	outInfo = float4(0.0, 0.0, zh, 0.0);
	if (DebugView == 1)
	{
		outColor = float4(bands(zh), 1.0);
		return;
	}
	if (DebugView == 2)
	{
		outColor = float4(world.a > 0.0 ? bands(zm) : host * 0.3, 1.0);
		return;
	}
	if (DebugView == 3)
	{
		outColor = float4(world.a > 0.0 ? float3(saturate((zh - zm) * 0.5 + 0.5), saturate(-(zh - zm) * 0.5 + 0.5), 0.0) : host * 0.3, 1.0);
		return;
	}

	// Steve: a little more depth allowance (his blocky shape is wider than GTA's character, in a car's seat too), and
	// glass in front of him (a car's window, a shop's) doesn't hide him
	const bool steve = steveH >= 0.0;
	// glass in front of him (a shop's window); and in a car's seat, above his waist, whatever of the car is close in front
	// of him (its windows, an open door's glass, the pillars): the car's own people show there too
	const bool glass = steve && zm > zh && ((SteveDepth.z > 0.0 && abs(zh - SteveDepth.z) < 0.6) ||
		(SteveUp.w > 0.5 && steveH > SteveMc.w * 0.42 && zm - zh < 1.4));
	const float visible = zm < zh + (steve ? max(allow, SteveBias) : allow) || glass ? 1.0 : 0.0;
	const float cover = world.a * visible;
	// (relit as straight colour, then put back over GTA's by its coverage)
	const float3 albedo = world.a > 1e-3 ? world.rgb / world.a : 0.0;
	const float3 lit = relight(albedo, uv, zm);
	outColor = float4((glass ? lerp(lit, host, 0.2) : lit) * cover + host * (1.0 - cover), 1.0);
	outInfo = float4(cover, cover > 0.0 ? zm : 0.0, zh, 0.0);
}

/// Minecraft's bright parts (lava, fire, the portal, blazes): what glows.
float4 PS_Bright(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const float cover = tex2D(sMcInfo, uv).x;
	const float3 c = tex2D(sComposite, uv).rgb;
	const float l = luma(c);
	return float4(c * (cover * saturate((l - BloomThreshold) / max(1.0 - BloomThreshold, 1e-3)) / max(l, 1e-3)), 1.0);
}

/// The finished picture: the world (shaken as one: GTA and Minecraft together), Minecraft's edges softened, contact
/// shadows on GTA's surfaces next to Minecraft's, Minecraft's glow, the portal's warp, and Minecraft's hand and HUD on
/// top (neither shaken nor warped).
float3 PS_Final(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	if (!McActive)
		return tex2D(ReShade::BackBuffer, uv).rgb;
	const float aspect = BUFFER_WIDTH * BUFFER_RCP_HEIGHT;
	const float t = Timer * 0.001;
	// shake: rotate and move the picture a little, zoomed in just enough that no edge shows
	const float amount = length(Shake.xy) + abs(Shake.z) * 0.5;
	const float zoom = 1.0 + amount * 2.2 + PortalWarp * 0.06;
	float2 d = (uv - 0.5) * float2(aspect, 1.0) / zoom;
	// the portal: a swirl and waves, strongest in the middle
	if (PortalWarp > 0.0)
	{
		const float r = length(d);
		const float swirl = PortalWarp * 1.1 * exp(-r * 2.2) * sin(t * 1.7 + 1.0);
		const float cs = cos(swirl), sn = sin(swirl);
		d = float2(d.x * cs - d.y * sn, d.x * sn + d.y * cs);
		d += PortalWarp * 0.014 * float2(sin(d.y * 38.0 + t * 7.0), cos(d.x * 31.0 + t * 6.0));
	}
	const float cr = cos(Shake.z), sr = sin(Shake.z);
	d = float2(d.x * cr - d.y * sr, d.x * sr + d.y * cr) + Shake.xy;
	const float2 wuv = d / float2(aspect, 1.0) + 0.5;

	const float2 px = BUFFER_PIXEL_SIZE;
	float3 color = tex2D(sComposite, wuv).rgb;
	const float4 info = tex2D(sMcInfo, wuv);
	// soften where Minecraft meets GTA (GTA's anti-aliasing never saw Minecraft)
	if (EdgeSoftness > 0.0)
	{
		const float c1 = tex2D(sMcInfo, wuv + float2(px.x, 0)).x, c2 = tex2D(sMcInfo, wuv - float2(px.x, 0)).x;
		const float c3 = tex2D(sMcInfo, wuv + float2(0, px.y)).x, c4 = tex2D(sMcInfo, wuv - float2(0, px.y)).x;
		const float edge = saturate(abs(c1 - info.x) + abs(c2 - info.x) + abs(c3 - info.x) + abs(c4 - info.x));
		if (edge > 0.0)
		{
			const float3 n = tex2D(sComposite, wuv + float2(px.x, 0)).rgb + tex2D(sComposite, wuv - float2(px.x, 0)).rgb
				+ tex2D(sComposite, wuv + float2(0, px.y)).rgb + tex2D(sComposite, wuv - float2(0, px.y)).rgb;
			color = lerp(color, (color * 2.0 + n) / 6.0, edge * EdgeSoftness);
		}
	}
	// contact shadows: GTA surface pixels with Minecraft surfaces close by (in depth), mostly above them
	if (ContactShadow > 0.0 && info.x < 0.5 && info.z < 200.0)
	{
		const float zh = info.z;
		const float r = clamp(ContactRadius / max(zh, 0.5) * BUFFER_HEIGHT / (2.0 * WarpTan.x), 2.0, 48.0);
		float occ = 0.0, weight = 0.0;
		[unroll] for (int i = 0; i < 10; ++i)
		{
			const float a = i * 2.39996 + 0.6; // golden-angle spiral
			const float rr = r * sqrt((i + 0.5) / 10.0);
			const float2 o = float2(cos(a), sin(a)) * rr;
			const float4 sm = tex2Dlod(sMcInfo, float4(wuv + o * px, 0, 0));
			const float w = o.y < 0.0 ? 1.0 : 0.35; // things stand on the ground: their shadow is below them
			weight += w;
			if (sm.x > 0.5 && abs(sm.y - zh) < ContactRadius * 1.5)
				occ += w * (1.0 - rr / (r + 1.0));
		}
		color *= 1.0 - ContactShadow * saturate(occ / max(weight, 1e-3) * 2.2);
	}
	// Minecraft's glow
	if (BloomStrength > 0.0)
	{
		const float3 b = tex2Dlod(sMcBright, float4(wuv, 0, 1.0)).rgb * 0.5 + tex2Dlod(sMcBright, float4(wuv, 0, 2.5)).rgb * 0.8
			+ tex2Dlod(sMcBright, float4(wuv, 0, 4.0)).rgb;
		color += b * BloomStrength;
	}
	// the portal's purple
	if (PortalWarp > 0.0)
	{
		const float r = length((uv - 0.5) * float2(aspect, 1.0));
		const float3 purple = color * float3(0.72, 0.38, 1.25) + float3(0.12, 0.0, 0.22) * (0.6 + 0.4 * sin(t * 3.0 + r * 9.0));
		color = lerp(color, purple, saturate(PortalWarp * (0.55 + r * 0.6)));
	}
	const float4 overlay = tex2D(sOverlay, float2(uv.x, 1.0 - uv.y)); // hand and HUD are screen-space: never shaken
	float3 result = overlay.rgb + saturate(color) * (1.0 - overlay.a);
	if (McCursor.z > 0.5)
	{
		// an arrow pointer (tip at the mouse), white with a black rim, sized for the screen height
		const float2 p = (uv - McCursor.xy) * float2(BUFFER_WIDTH, BUFFER_HEIGHT) / max(BUFFER_HEIGHT / 1080.0, 0.5);
		const float edge = 12.7 * (p.y - 18.0) + 5.3 * p.x; // the arrow's lower edge, (0, 18) to (12.7, 12.7): <= 0 inside
		if (p.x >= 0.0 && p.y >= p.x && edge <= 0.0)
			result = (p.x >= 1.4 && p.y >= p.x + 2.0 && edge <= -19.3) ? float3(1.0, 1.0, 1.0) : float3(0.0, 0.0, 0.0);
	}
	return result;
}

technique MCPassthrough < ui_tooltip = "Minecraft passthrough: enabled automatically while the Minecraft link is up."; >
{
	pass Light
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Light;
		RenderTarget0 = GtaLightTex;
		RenderTarget1 = GtaStatTex;
	}
	pass Composite
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Composite;
		RenderTarget0 = CompositeTex;
		RenderTarget1 = McInfoTex;
	}
	pass Bright
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Bright;
		RenderTarget = McBrightTex;
	}
	pass Final
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Final;
	}
}
