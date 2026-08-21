#version 450

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColour;
layout(location = 2) in vec4 inLightPosition;
layout(location = 3) in vec4 inSurfacePosition;
layout(location = 4) in vec2 inTexCoord;
layout(location = 5) in vec3 inWorldPosition;

layout(location = 0) out vec4 outColour;

// Fragment samplers are set 2 for SPIR-V; uniform buffers are set 3.
layout(set = 2, binding = 0) uniform sampler2D shadowMap;
layout(set = 2, binding = 1) uniform sampler2D surfaceMap;
layout(set = 2, binding = 2) uniform sampler2D colourMap;

// The beams: up to four holes' worth of shadow, in one 2x2 atlas.
//
// **A hole carries occlusion and not light.** Both rooms already have the
// world's sun, so what crosses a portal is the *absence* of it - a caster in
// front of a hole darkens the floor beyond it. Taking the darker of the two is
// what makes that coherent with one global sun; adding a second contribution
// would double-light every floor near a doorway.
layout(set = 2, binding = 3) uniform sampler2D beamMap;
layout(set = 2, binding = 4) uniform sampler2D normalMap;
layout(set = 2, binding = 5) uniform sampler2D roughnessMap;
layout(set = 2, binding = 6) uniform sampler2D occlusionMap;
layout(set = 2, binding = 7) uniform sampler2D emissiveMap;
layout(set = 2, binding = 8) uniform sampler2D heightMap;

layout(set = 3, binding = 0) uniform Lighting {
	vec4 Direction;
	vec4 Ambient;
	vec4 Direct;

	// x: 1 when a shadow map was rendered this frame, 0 otherwise.
	// y: one texel of the shadow map, for the sample offsets.
	// z: how this draw reads `surfaceMap`. 0 draws its own tint; 1 projects
	//    through `SurfaceViewProjection` and is a **mirror**; 2 reads by screen
	//    position and is a **portal**; 3 is a portal at the **end of the
	//    recursion**, which has no image to read and shades flat. See
	//    `SurfacePane` below for why the first two are two lookups rather than
	//    one.
	// w: how opaque the projected image is, 0 to 1. See the composite below.
	vec4 Flags;

	// The submesh's own colour, multiplied into whatever the texture gives.
	// White for a draw that has no material of its own.
	vec4 BaseColour;

	// x: 1 when `colourMap` holds this draw's texture rather than the one-texel
	//    stand-in.
	// y: the alpha below which a fragment is discarded, or 0 to discard none.
	// z: 1 when a height map is present. w: its world-independent UV scale.
	vec4 Surface;

	// Presence of normal, roughness, occlusion, and emissive data maps.
	vec4 Material;

	// Where the current animation cell sits in its sheet: x the scale, yz the
	// offset. The identity (1, 0, 0) for a texture that is not a sheet, so this
	// is applied unconditionally rather than behind a branch - a divergent
	// branch per fragment to avoid a multiply and an add is the wrong trade.
	vec4 Flipbook;

	// x: which `scene::SurfaceEffect` the projected image goes through.
	// y: the animation clock, for the effects that move.
	// z: 1 when this draw is being captured into a surface texture that will be
	//    sampled back as a *display-encoded* colour, so the output has to be
	//    tonemapped here. See `Encode` below.
	vec4 Mirror;

	// The face normal of the pane this draw is, for a portal, and zero
	// otherwise.
	//
	// **A pane is a box and a hole is a rectangle.** Without this the pane's
	// four edge faces read the sub-render too, so a hole comes out as wide as
	// the slab it is cut in with a band of the far room round its rim at a
	// parallax nothing else in the frame has. The rim falls through to the
	// pane's own material instead, which is what a frame is.
	vec4 PaneNormal;

	// The half-space this draw keeps: xyz a unit world normal, w the offset
	// along it. A zero normal keeps everything, which is every draw in a world
	// with no portal in it.
	//
	// **A body standing in a hole is one body cut at the plane.** Its far half
	// is drawn as a second instance in the room beyond, and without the cut both
	// are drawn whole - the original hanging out of the back of the pane and the
	// copy out of the far one. See `scene::DrawInstance::SeamNormal`.
	vec4 SeamPlane;

	// The sky term and eye-relative fog. Kept in this per-draw block because a
	// mirror or portal pass has its own eye even when it shares the world.
	vec4 OutdoorAmbient;
	vec4 FogColour;
	// x: start distance. y: complete distance.
	vec4 Fog;
	vec4 Eye;
} lighting;

// The tonemap, for a pass whose output is sampled rather than presented.
//
// **Why a mirror needs this and the main view does not.** The main view is
// deferred: `gbuffer` writes surfaces, `deferred-lighting` writes radiance into
// a float target, and the `tonemap` node encodes that for display. A mirror is
// forward - `mirror-capture` runs this shader straight into the surface texture
// - and nothing encodes it afterwards, because there is no `mirror-tonemap`
// node the way there is a `portal-tonemap` one.
//
// The pane then samples that texture and treats it as a display colour, so it
// runs it back through `WorkingFromDisplay`, and the frame's own tonemap
// encodes the result a second time. A linear value put through that round trip
// comes out about a fifth as bright, which is exactly what a mirror measured:
// 0.0588 against 0.2843 for the same floor seen directly.
//
// Encoding at capture makes the round trip an identity. The same maths as
// `tonemap.frag`, deliberately - two tonemaps that drift apart would show as a
// mirror whose colours are close but not right, which is harder to see and
// harder to explain than one that is plainly too dark.
vec3 Aces(vec3 value) {
	return clamp(
		(value * (2.51 * value + 0.03)) / (value * (2.43 * value + 0.59) + 0.14),
		0.0,
		1.0
	);
}

vec3 Encode(vec3 lit) {
	// **Only when asked.** `portal-capture` runs this same shader and *does*
	// have a `portal-tonemap` node after it, so encoding unconditionally would
	// tonemap a portal twice. `transparent` runs it too, straight onto the
	// already-encoded frame - that one is un-tonemapped for the same reason a
	// mirror was, and is a separate fix rather than a rider on this one.
	if (lighting.Mirror.z < 0.5) {
		return lit;
	}
	return pow(Aces(max(lit, vec3(0.0))), vec3(1.0 / 2.2));
}

float FogFactor() {
	float interval = max(lighting.Fog.y - lighting.Fog.x, 0.0001);
	return clamp((distance(inWorldPosition, lighting.Eye.xyz) - lighting.Fog.x) / interval, 0.0, 1.0);
}

// The effect ordinals, which are `scene::SurfaceEffect`'s and are the format.
//
// **Spelled here as well as there, and that is the one duplication in this
// pair.** A shader cannot include a C++ header; what stops the two drifting is
// that the enum's own comment says the ordinals are on the wire and may only be
// appended to, and that the four below are checked by looking at a mirror.
#define EFFECT_NIGHT_VISION 1
#define EFFECT_THERMAL      2
#define EFFECT_CCTV         3
#define EFFECT_SWIRL        4

// A cheap hash, for the grain the two sensor effects want.
//
// Not a texture: a noise lookup would be a fourth sampler bound on every draw
// in the frame so that two mirrors can be grainy.
float MirrorNoise(vec2 at) {
	return fract(sin(dot(at, vec2(12.9898, 78.233))) * 43758.5453);
}

// Where a fragment reads the surface texture, once the effect has had its say.
//
// **Only `Swirl` moves texels**, so this is a branch that almost always falls
// through - and it has to be separate from the grade below because a warp
// happens before the fetch and a grade happens after it.
vec2 MirrorLookup(vec2 uv, float effect, float seconds) {
	if (effect < float(EFFECT_SWIRL) - 0.5) {
		return uv;
	}

	// About the middle of the pane, falling off to nothing at every edge. Time
	// changes the amount of twist instead of adding a rotation shared by every
	// texel, which keeps the pane's frame anchored to the world.
	vec2 centred = uv - 0.5;
	float radius = length(centred);
	vec2 edgeDistance = min(uv, vec2(1.0) - uv);
	float boundary = smoothstep(0.0, 0.30, min(edgeDistance.x, edgeDistance.y));
	float centre = 1.0 - smoothstep(0.08, 0.72, radius);
	float twist = boundary * centre * (2.6 + sin(seconds * 0.55) * 0.65);

	float sine = sin(twist);
	float cosine = cos(twist);
	vec2 turned = vec2(
		centred.x * cosine - centred.y * sine,
		centred.x * sine + centred.y * cosine
	);
	return clamp(turned + 0.5, vec2(0.0), vec2(1.0));
}

// The heat ramp `Thermal` reads luminance through.
//
// Five stops, black through blue, magenta, red and yellow to white - the ramp
// every thermal camera ships with, because it is the one that keeps its
// ordering legible to somebody who has never seen one before.
vec3 ThermalRamp(float level) {
	if (level < 0.12) {
		return mix(vec3(0.0), vec3(0.0, 0.05, 0.42), smoothstep(0.0, 0.12, level));
	}
	if (level < 0.32) {
		return mix(
			vec3(0.0, 0.05, 0.42), vec3(0.58, 0.0, 0.72), smoothstep(0.12, 0.32, level)
		);
	}
	if (level < 0.54) {
		return mix(
			vec3(0.58, 0.0, 0.72), vec3(0.96, 0.04, 0.04), smoothstep(0.32, 0.54, level)
		);
	}
	if (level < 0.78) {
		return mix(
			vec3(0.96, 0.04, 0.04), vec3(1.0, 0.86, 0.02), smoothstep(0.54, 0.78, level)
		);
	}
	return mix(vec3(1.0, 0.86, 0.02), vec3(1.0), smoothstep(0.78, 1.0, level));
}

float MirrorVignette(float edge, float inner, float outer) {
	return 1.0 - smoothstep(inner, outer, edge);
}

// What the pane shows, once the sampled image has been graded.
//
// **A grade over an image that is already rendered**, which is the whole reason
// these are affordable: the surface pass draws the world exactly as it would
// have, and every effect here is a handful of instructions on the fragment that
// samples it.
vec3 MirrorGrade(vec3 image, vec2 uv, float effect, float seconds) {
	float level = dot(image, vec3(0.2126, 0.7152, 0.0722));

	// Distance from the middle, which three of the four want for a vignette. A
	// sensor image that reached the corners as brightly as the centre is the
	// one thing that stops any of these reading as a screen.
	float edge = length(uv - 0.5);

	if (effect < float(EFFECT_NIGHT_VISION) + 0.5) {
		// Night vision: an intensifier, so the gain comes first and the tint
		// second. Lifted, because the point of one is that a dark scene stops
		// being dark - but not as hard as an intensifier really does. A gain
		// that saturated the mid-tones made a lit room a flat green rectangle,
		// which is a filter rather than a picture: what makes this read as a
		// scope is that the shapes survive it.
		float gained = max(1.0 - exp(-max(level, 0.0) * 5.5), 0.07);

		// Grain that moves. A still grain reads as dirt on the glass.
		float grain = MirrorNoise(uv * 640.0 + seconds * 37.0);
		gained = clamp(gained + (grain - 0.5) * 0.08, 0.0, 1.0);

		float lines = 0.94 + 0.06 * sin(uv.y * 900.0);
		float vignette = mix(0.38, 1.0, MirrorVignette(edge, 0.34, 0.70));
		return vec3(gained * 0.18, gained, gained * 0.30) * lines * vignette;
	}

	if (effect < float(EFFECT_THERMAL) + 0.5) {
		// Thermal: luminance stands in for temperature, which is a lie an
		// engine with no thermal model cannot avoid - `scene::SurfaceEffect`
		// says so rather than implying otherwise. It reads right because bright
		// things in a lit scene usually are the hot ones.
		float temperature = pow(clamp(level * 1.8, 0.0, 1.0), 0.62);
		return ThermalRamp(temperature);
	}

	if (effect < float(EFFECT_CCTV) + 0.5) {
		// A security camera: grey, contrast-crushed, scanlined, and with a
		// bright band rolling down it. The band is what makes it read as a
		// *feed* rather than as a black-and-white photograph.
		float grey = clamp((level - 0.5) * 1.35 + 0.5, 0.0, 1.0);

		float lines = 0.82 + 0.18 * sin(uv.y * 620.0);
		float grain = MirrorNoise(uv * 380.0 + seconds * 53.0);
		float band = 1.0 - smoothstep(0.0, 0.10, abs(fract(uv.y + seconds * 0.22) - 0.5));
		float vignette = mix(0.45, 1.0, MirrorVignette(edge, 0.36, 0.72));

		float value = clamp(grey * lines + (grain - 0.5) * 0.07 + band * 0.16, 0.0, 1.0);

		// Faintly warm rather than neutral, which is what a cheap sensor's
		// white balance does and what stops this looking like a greyscale
		// filter.
		return vec3(value * 1.02, value, value * 0.94) * vignette;
	}

	// Swirl grades nothing. The warp already happened, in `MirrorLookup`.
	return image;
}

// How many local lights one draw may be affected by.
//
// **Sixteen, and it is a forward renderer's simplest form**, which is what
// `RENDER_PIPELINE.md` puts a clustered pass at v0.11 to replace. Every fragment
// tests every light in the buffer, so this is a cost paid per pixel per light
// whether the light reaches it or not - the range check below is a branch, not a
// skip, because the loop bound is uniform.
//
// Sixteen because a room has a handful of lamps and because a fourth `vec4` per
// light would put the buffer past what some drivers keep in fast memory.
//
// **The number is not spelled here.** The build passes `-DMAX_LIGHTS` read out
// of `render::MAX_SCENE_LIGHTS`, which sizes `LightUniforms` on the C++ side -
// so the array below and the buffer it is fed from cannot disagree. A cap
// smaller than the uniform silently ignores the tail of the set; a larger one
// reads past it. Both look like "that lamp does not work", which is why this is
// arranged so neither can happen rather than checked afterwards.
//
// The fallback is for compiling this file by hand, and is never what the build
// uses.
#ifndef MAX_LIGHTS
	#define MAX_LIGHTS 16
#endif

// The point, spot and surface lights near this draw.
//
// **A second uniform buffer pushed once per pass rather than fields on
// `Lighting`, which is pushed per draw.** `Lighting` carries the submesh's base
// colour and the surface flags, so it changes every draw call; the light set does
// not change within a frame. Folding them together would mean re-uploading 768
// bytes of light data on every one of a scene's draw calls to say the same thing.
layout(set = 3, binding = 1) uniform Lights {
	// xyz: where it is, in world space. w: how far it reaches, in metres.
	vec4 Position[MAX_LIGHTS];

	// rgb: colour times brightness. w: unused, named so the size is stated.
	vec4 Colour[MAX_LIGHTS];

	// xyz: which way a spot points. w: the cosine of its half-angle, or -1 for a
	// point light - **a cosine rather than the angle**, because the test is a dot
	// product and converting per fragment per light would be sixteen `acos` calls
	// a pixel.
	vec4 Direction[MAX_LIGHTS];

	// x: how many of the arrays are in use. The rest is named so the struct's
	// size is stated rather than implied.
	vec4 Count;
} lights;

// How many holes may carry a shadow in one frame. `render::MAX_PORTAL_BEAMS`.
#define MAX_BEAMS 4

// One hole's beam, and what a far-side fragment has to go through to read it.
//
// **The casters are left in the near room and the receiver is mapped back**,
// which is the whole reason this costs two matrix products rather than a second
// copy of the world. `Back` carries a fragment from the far side of a hole to
// the near side; `Plane` is what says it was on the far side to begin with; and
// `Light` is the beam's own matrix, fitted to the pane's rectangle so that the
// frustum *is* the aperture - a fragment the beam does not reach projects
// outside `0..1`, which the range check below already reads as lit.
layout(set = 3, binding = 2) uniform Beams {
	mat4 Light[MAX_BEAMS];
	mat4 Back[MAX_BEAMS];

	// xyz the near pane's normal, w its offset along it.
	vec4 Plane[MAX_BEAMS];

	// xy the scale into the atlas, zw the offset.
	vec4 Region[MAX_BEAMS];

	// x: how many are in use.
	vec4 Count;
} beams;

// How much of the sun reaches this fragment through the holes in the world.
//
// One, meaning unshadowed, for every fragment in every scene with no portal in
// it - the loop ends at its first test.
float BeamFactor() {
	float lit = 1.0;

	int count = int(beams.Count.x);
	for (int index = 0; index < MAX_BEAMS; index++) {
		if (index >= count) {
			break;
		}

		// **Behind the near pane's face is the far side**, which is where a
		// fragment has to have come from for this beam to say anything about it.
		// A fragment already in the near room is shadowed by the world's own map
		// and would otherwise be shadowed twice.
		vec4 back = beams.Back[index] * vec4(inWorldPosition, 1.0);
		vec3 near = back.xyz / max(back.w, 1e-6);

		if (dot(near, beams.Plane[index].xyz) <= beams.Plane[index].w) {
			continue;
		}

		vec4 lightPosition = beams.Light[index] * vec4(near, 1.0);
		vec3 projected = lightPosition.xyz / max(lightPosition.w, 1e-6);
		if (projected.z > 1.0 || projected.z < 0.0) {
			continue;
		}

		// The same convention the world map's lookup uses, then folded into this
		// beam's quadrant of the atlas.
		vec2 uv = vec2(projected.x * 0.5 + 0.5, 0.5 - projected.y * 0.5);
		if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
			continue;
		}

		vec2 atlas = uv * beams.Region[index].xy + beams.Region[index].zw;

		// **One tap, where the world map takes four.** A beam's edge is the
		// hole's own rim, which is a hard edge in the geometry as well - a soft
		// one there would read as the hole being out of focus.
		float closest = texture(beamMap, atlas).r;
		lit = min(lit, (projected.z - 0.0025) <= closest ? 1.0 : 0.0);
	}

	return lit;
}

// What the local lights add at this fragment.
//
// **Added to the directional term rather than replacing it**, so a scene with no
// lamps in it looks exactly as it did before v0.10 - which is the property that
// makes this safe to turn on for every existing world.
vec3 LocalLight(vec3 normal) {
	vec3 total = vec3(0.0);
	int count = int(lights.Count.x);

	for (int index = 0; index < MAX_LIGHTS; index++) {
		if (index >= count) {
			break;
		}

		vec3 offset = lights.Position[index].xyz - inWorldPosition;
		float range = lights.Position[index].w;
		float distance = length(offset);
		if (distance > range || range <= 0.0) {
			continue;
		}

		vec3 toLight = offset / max(distance, 1e-4);
		float lambert = max(dot(normal, toLight), 0.0);
		if (lambert <= 0.0) {
			continue;
		}

		// **Inverse-square, windowed to reach zero at the range.** A bare inverse
		// square never reaches zero, so a light would pop off at its cutoff; the
		// squared window is the standard fix and costs two multiplies.
		float ratio = distance / range;
		float window = max(1.0 - ratio * ratio, 0.0);
		float falloff = window * window / (1.0 + distance * distance);

		// A cone, when the light has one. `-1` never clips, which is what a point
		// light stores rather than needing a branch of its own.
		float cosine = lights.Direction[index].w;
		if (cosine > -1.0) {
			float aligned = dot(-toLight, normalize(lights.Direction[index].xyz));
			if (aligned < cosine) {
				continue;
			}
			// Softened over the outer tenth of the cone, so the edge is not a
			// hard-cut circle.
			falloff *= smoothstep(cosine, mix(cosine, 1.0, 0.1), aligned);
		}

		total += lights.Colour[index].rgb * (lambert * falloff);
	}

	return total;
}

// How much light reaches this fragment, 0 fully shadowed to 1 fully lit.
float ShadowFactor(vec3 normal, vec3 toLight) {
	if (lighting.Flags.x < 0.5) {
		return 1.0;
	}

	// Divide here so the lookup remains perspective-correct if the projection changes.
	vec3 projected = inLightPosition.xyz / inLightPosition.w;

	// SDL's viewport convention maps the top of the target to texture v = 0.
	vec2 uv = vec2(projected.x * 0.5 + 0.5, 0.5 - projected.y * 0.5);

	// Outside the map is lit, not shadowed. The map covers what the scene
	// reaches; a fragment past its edge is one the light was never fitted to,
	// and shadowing it would put a hard black border around the world.
	if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || projected.z > 1.0) {
		return 1.0;
	}

	// Scale the bias with the light angle to reduce acne without detached shadows.
	float slope = 1.0 - max(dot(normal, toLight), 0.0);
	float bias = 0.0015 + 0.0045 * slope;

	// Four taps in a rotated square. Not sixteen: this is a soft edge rather
	// than a filter, and the difference between four and one is most of what a
	// viewer notices while the difference between four and sixteen is not.
	float texel = lighting.Flags.y;
	vec2 offsets[4] = vec2[4](
		vec2(-0.5, -0.5) * texel,
		vec2(0.5, -0.5) * texel,
		vec2(-0.5, 0.5) * texel,
		vec2(0.5, 0.5) * texel
	);

	float lit = 0.0;
	for (int index = 0; index < 4; index++) {
		float closest = texture(shadowMap, uv + offsets[index]).r;
		lit += (projected.z - bias) <= closest ? 1.0 : 0.0;
	}
	return lit * 0.25;
}

mat3 CotangentFrame(vec3 normal, vec3 position, vec2 uv) {
	vec3 positionX = dFdx(position);
	vec3 positionY = dFdy(position);
	vec2 uvX = dFdx(uv);
	vec2 uvY = dFdy(uv);
	vec3 tangent = positionX * uvY.y - positionY * uvX.y;
	vec3 bitangent = -positionX * uvY.x + positionY * uvX.x;
	float scale = inversesqrt(max(max(dot(tangent, tangent), dot(bitangent, bitangent)), 1e-8));
	return mat3(tangent * scale, bitangent * scale, normal);
}

float DistributionGGX(vec3 normal, vec3 halfway, float roughness) {
	float alpha = roughness * roughness;
	float alphaSquared = alpha * alpha;
	float normalHalf = max(dot(normal, halfway), 0.0);
	float denominator = normalHalf * normalHalf * (alphaSquared - 1.0) + 1.0;
	return alphaSquared / max(3.14159265 * denominator * denominator, 1e-5);
}

float GeometrySchlickGGX(float cosine, float roughness) {
	float radius = roughness + 1.0;
	float factor = radius * radius * 0.125;
	return cosine / max(cosine * (1.0 - factor) + factor, 1e-5);
}

vec3 FresnelSchlick(float cosine, vec3 base) {
	return base + (1.0 - base) * pow(clamp(1.0 - cosine, 0.0, 1.0), 5.0);
}

void main() {
	vec3 normal = normalize(inNormal);
	vec3 toLight = -normalize(lighting.Direction.xyz);
	float lambert = max(dot(normal, toLight), 0.0);

	// **The three colour sources multiply rather than one winning.** The
	// texture is what the artist painted, the base colour is what the *material*
	// says that run is - which is the whole of an untextured import - and the
	// instance tint is what the scene says this copy of it is. A pipeline that
	// let any of them replace the others would lose a different thing in each
	// of the three cases.
	// **`fract` before the cell transform, so a tiled coordinate stays inside its
	// cell.** An imported mesh's UVs routinely run past one and the sampler
	// repeats them, which is right for a whole sheet and catastrophic for a cell:
	// without this a coordinate of 1.2 would land two cells further along and a
	// GIF on a tiled surface would show several frames at once.
	vec2 localUv = fract(inTexCoord);
	vec2 cellUv = localUv * lighting.Flipbook.x + lighting.Flipbook.yz;
	if (lighting.Surface.z > 0.5) {
		mat3 tangentFrame = CotangentFrame(normal, inWorldPosition, cellUv);
		vec3 tangentEye = transpose(tangentFrame) * normalize(lighting.Eye.xyz - inWorldPosition);
		float height = texture(heightMap, cellUv).r - 0.5;
		float grazing = max(abs(tangentEye.z), 0.2);
		localUv = fract(localUv - tangentEye.xy * (height * lighting.Surface.w / grazing));
		cellUv = localUv * lighting.Flipbook.x + lighting.Flipbook.yz;
	}
	if (lighting.Material.x > 0.5) {
		vec3 mappedNormal = texture(normalMap, cellUv).xyz * 2.0 - 1.0;
		normal = normalize(CotangentFrame(normal, inWorldPosition, cellUv) * mappedNormal);
		toLight = -normalize(lighting.Direction.xyz);
		lambert = max(dot(normal, toLight), 0.0);
	}

	// The outdoor term follows the final material normal. Computing it before
	// the normal map made raised detail receive the sky light of the flat mesh.
	float sky = max(normal.y, 0.0);

	vec4 sampled = lighting.Surface.x > 0.5 ? texture(colourMap, cellUv) : vec4(1.0);
	float roughness = lighting.Material.y > 0.5 ? texture(roughnessMap, cellUv).r : 0.65;
	roughness = clamp(roughness, 0.045, 1.0);
	float occlusion = lighting.Material.z > 0.5 ? texture(occlusionMap, cellUv).r : 1.0;
	vec3 emissive = lighting.Material.w > 0.5 ? texture(emissiveMap, cellUv).rgb : vec3(0.0);

	// Cut-out before anything else is computed. A hair card is authored as a
	// plane with a mask, and discarding is what keeps it opaque and out of the
	// sorted pass - see `scene::AlphaMode`.
	float alpha = inColour.a * sampled.a * lighting.BaseColour.a;
	if (lighting.Surface.y > 0.0 && alpha < lighting.Surface.y) {
		discard;
	}

	// **The seam, before any shading is computed.** A fragment on the far side of
	// the plane this instance was cut at belongs to the other half of the body,
	// which is drawn as its own instance in the room through the hole.
	//
	// **A discard rather than a clip plane**, because this pipeline declares no
	// clip-distance slot and the draws this applies to are a handful of
	// instances a frame - the run breaks wherever the plane changes, so it is
	// only ever the halves that pay for the branch. It does defeat early-Z on
	// those draws, which is why what may be cut is bounded by what fits through
	// the hole.
	if (dot(lighting.SeamPlane.xyz, lighting.SeamPlane.xyz) > 0.0 &&
		dot(inWorldPosition, lighting.SeamPlane.xyz) < lighting.SeamPlane.w) {
		discard;
	}

	vec3 albedo = inColour.rgb * sampled.rgb * lighting.BaseColour.rgb;

	// Ambient is unshadowed and direct light is not, which is what makes a
	// shadow dark rather than black.
	// **The darker of the two, never the sum.** The world's map says what this
	// room's own geometry blocks and a beam says what the room through a hole
	// blocks; light that fails either test does not arrive.
	float shadow = min(ShadowFactor(normal, toLight), BeamFactor());
	vec3 viewDirection = normalize(lighting.Eye.xyz - inWorldPosition);
	vec3 halfway = normalize(viewDirection + toLight);
	vec3 fresnel = FresnelSchlick(max(dot(halfway, viewDirection), 0.0), vec3(0.04));
	float distribution = DistributionGGX(normal, halfway, roughness);
	float geometry = GeometrySchlickGGX(max(dot(normal, viewDirection), 0.0), roughness) *
		GeometrySchlickGGX(lambert, roughness);
	vec3 specular = distribution * geometry * fresnel /
		max(4.0 * max(dot(normal, viewDirection), 0.0) * lambert, 1e-4);
	vec3 ambient = albedo * (lighting.Ambient.rgb + lighting.OutdoorAmbient.rgb * sky) * occlusion;
	vec3 direct = (albedo * (1.0 - fresnel) * lambert + specular) * lighting.Direct.rgb * shadow;
	vec3 lit = ambient + direct + albedo * LocalLight(normal) + emissive;
	lit = mix(lit, lighting.FogColour.rgb, FogFactor());

	// **A portal pane reads the sub-render by screen position**, which is the
	// whole of CodeParade's `portal.frag` and the reason the recursive pass can
	// be pixel-exact where the surface path cannot. The sub-view is rendered
	// with the screen's own projection into a target the size of this
	// attachment, so this fragment's own position in the target *is* the texel
	// it wants - one texel per pixel at every distance, with nothing fitted and
	// nothing to go coarse when you walk into the hole.
	//
	// `gl_FragCoord.xy` is already in the target's pixels and already points the
	// way `v` does, so there is no flip here and no `w` divide: the demo's
	// `gl_Position.xy / w` is the same quantity arrived at through a varying,
	// which this pipeline does not need because the rasteriser hands it over.
	//
	// **Never rejected for being outside 0..1**, unlike the mirror below. It
	// cannot be - the target covers the same rectangle as the frame this
	// fragment is in - and a bounds test would only be a way to fail on a
	// rounding error at the very edge of the pane.
	// **The end of the chain, which is a shade and not a wall.** A hole at the
	// deepest level the recursion goes to has no sub-render to sample, and
	// drawing its own lit material there puts a grey slab at the end of a
	// corridor of holes - the one thing a corridor of holes must not look like.
	// The ambient is the far room's own unlit tone, so the chain fades into it
	// rather than stopping against something.
	if (lighting.Flags.z > 2.5) {
		outColour = vec4(Encode(lighting.Ambient.rgb), max(alpha, lighting.Flags.w));
		return;
	}

	// **Both faces and neither rim.** A hole is a hole from either side - the
	// warp already answers which - so the test is on the axis rather than on the
	// sign, and a face square to the pane's own normal shows the picture while
	// one across it does not. A zero normal is every draw that is not a portal
	// pane, and it accepts everything.
	const bool onPane = dot(lighting.PaneNormal.xyz, lighting.PaneNormal.xyz) <= 0.0 ||
		abs(dot(normalize(inNormal), lighting.PaneNormal.xyz)) > 0.5;

	if (lighting.Flags.z > 1.5 && onPane) {
		vec2 portalUv = gl_FragCoord.xy / vec2(textureSize(surfaceMap, 0));

		// **Untinted, unlike the mirror below, because a hole is not glass.**
		// A coloured mirror is a real thing and multiplying its image by the
		// pane's own colour is how it is authored; a portal showing the room
		// beyond dimmed by whatever grey the pane happens to be is a trap every
		// scene walks into, because a pane is grey by default and forty-five per
		// cent of the far room's light disappears without anything saying so.
		// What you see through a hole is what is there. `ImageTransparency` is
		// still how a pane is faded towards its own material.
		vec3 image = texture(surfaceMap, portalUv).rgb;

		float imageAlpha = lighting.Flags.w;
		outColour = vec4(Encode(mix(lit, image, imageAlpha)), max(alpha, imageAlpha));
		return;
	}

	// **The surface texture, projected from the camera that rendered it.** A
	// planar projection is exactly right for a flat mirror and exactly wrong
	// for anything else, which is why this is a mirror feature rather than a
	// general one - see `SurfaceCamera` on the C++ side.
	if (lighting.Flags.z > 0.5) {
		vec3 surface = inSurfacePosition.xyz / max(inSurfacePosition.w, 1e-6);

		// Y flipped for the reason the shadow lookup gives. This one is visible
		// rather than subtle: the mirror showed the world upside down.
		vec2 surfaceUv = vec2(surface.x * 0.5 + 0.5, 0.5 - surface.y * 0.5);

		if (surfaceUv.x >= 0.0 && surfaceUv.x <= 1.0 && surfaceUv.y >= 0.0 && surfaceUv.y <= 1.0) {
			// **The effect gets the coordinate before the fetch and the colour
			// after it.** A swirl moves texels and the three sensors grade
			// them, and doing either at the other's moment is doing it to the
			// wrong thing.
			const float effect = lighting.Mirror.x;
			const float seconds = lighting.Mirror.y;

			// Tinted rather than replaced, so a coloured mirror is possible and
			// a white one is unchanged.
			//
			// A mirror is not lit by the scene: what it shows is already lit.
			vec3 image = texture(surfaceMap, MirrorLookup(surfaceUv, effect, seconds)).rgb * inColour.rgb;
			if (effect > 0.5) {
				image = MirrorGrade(image, surfaceUv, effect, seconds);
			}

			float imageAlpha = lighting.Flags.w;

			// Preserve the image opacity independently from the pane transparency.
			outColour = vec4(Encode(mix(lit, image, imageAlpha)), max(alpha, imageAlpha));
			return;
		}
	}

	outColour = vec4(Encode(lit), alpha);
}
