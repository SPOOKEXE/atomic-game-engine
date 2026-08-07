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

layout(set = 3, binding = 0) uniform Lighting {
	vec4 Direction;
	vec4 Ambient;

	// x: 1 when a shadow map was rendered this frame, 0 otherwise.
	// y: one texel of the shadow map, for the sample offsets.
	// z: 1 when this draw samples the surface texture rather than its own tint.
	// w: how opaque the projected image is, 0 to 1. See the composite below.
	vec4 Flags;

	// The submesh's own colour, multiplied into whatever the texture gives.
	// White for a draw that has no material of its own.
	vec4 BaseColour;

	// x: 1 when `colourMap` holds this draw's texture rather than the one-texel
	//    stand-in.
	// y: the alpha below which a fragment is discarded, or 0 to discard none.
	vec4 Surface;
} lighting;

// How many local lights one draw may be affected by.
//
// **Sixteen, and it is a forward renderer's simplest form**, which is what
// `RENDER_PIPELINE.md` puts a clustered pass at v0.11 to replace. Every fragment
// tests every light in the buffer, so this is a cost paid per pixel per light
// whether the light reaches it or not — the range check below is a branch, not a
// skip, because the loop bound is uniform.
//
// Sixteen because a room has a handful of lamps and because a fourth `vec4` per
// light would put the buffer past what some drivers keep in fast memory.
const int MAX_LIGHTS = 16;

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
	// point light — **a cosine rather than the angle**, because the test is a dot
	// product and converting per fragment per light would be sixteen `acos` calls
	// a pixel.
	vec4 Direction[MAX_LIGHTS];

	// x: how many of the arrays are in use. The rest is named so the struct's
	// size is stated rather than implied.
	vec4 Count;
} lights;

// What the local lights add at this fragment.
//
// **Added to the directional term rather than replacing it**, so a scene with no
// lamps in it looks exactly as it did before v0.10 — which is the property that
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

void main() {
	vec3 normal = normalize(inNormal);
	vec3 toLight = -normalize(lighting.Direction.xyz);
	float lambert = max(dot(normal, toLight), 0.0);

	// A weak upward bounce, so faces pointing away from the light are shaded
	// rather than flat. Cheaper than a second light and enough to read shape.
	float bounce = max(normal.y, 0.0) * 0.15;

	// **The three colour sources multiply rather than one winning.** The
	// texture is what the artist painted, the base colour is what the *material*
	// says that run is — which is the whole of an untextured import — and the
	// instance tint is what the scene says this copy of it is. A pipeline that
	// let any of them replace the others would lose a different thing in each
	// of the three cases.
	vec4 sampled = lighting.Surface.x > 0.5 ? texture(colourMap, inTexCoord) : vec4(1.0);

	// Cut-out before anything else is computed. A hair card is authored as a
	// plane with a mask, and discarding is what keeps it opaque and out of the
	// sorted pass — see `scene::AlphaMode`.
	float alpha = inColour.a * sampled.a * lighting.BaseColour.a;
	if (lighting.Surface.y > 0.0 && alpha < lighting.Surface.y) {
		discard;
	}

	vec3 albedo = inColour.rgb * sampled.rgb * lighting.BaseColour.rgb;

	// Ambient is unshadowed and direct light is not, which is what makes a
	// shadow dark rather than black.
	float shadow = ShadowFactor(normal, toLight);
	vec3 lit = albedo * (lighting.Ambient.rgb + vec3(lambert * shadow + bounce) + LocalLight(normal));

	// **The surface texture, projected from the camera that rendered it.** A
	// planar projection is exactly right for a flat mirror and exactly wrong
	// for anything else, which is why this is a mirror feature rather than a
	// general one — see `SurfaceCamera` on the C++ side.
	if (lighting.Flags.z > 0.5) {
		vec3 surface = inSurfacePosition.xyz / max(inSurfacePosition.w, 1e-6);

		// Y flipped for the reason the shadow lookup gives. This one is visible
		// rather than subtle: the mirror showed the world upside down.
		vec2 surfaceUv = vec2(surface.x * 0.5 + 0.5, 0.5 - surface.y * 0.5);

		if (surfaceUv.x >= 0.0 && surfaceUv.x <= 1.0 && surfaceUv.y >= 0.0 && surfaceUv.y <= 1.0) {
			// Tinted rather than replaced, so a coloured mirror is possible and
			// a white one is unchanged.
			//
			// A mirror is not lit by the scene: what it shows is already lit.
			vec3 image = texture(surfaceMap, surfaceUv).rgb * inColour.rgb;
			float imageAlpha = lighting.Flags.w;

			// Preserve the image opacity independently from the pane transparency.
			outColour = vec4(mix(lit, image, imageAlpha), max(alpha, imageAlpha));
			return;
		}
	}

	outColour = vec4(lit, alpha);
}
