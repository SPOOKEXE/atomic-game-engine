#version 450

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColour;
layout(location = 2) in vec4 inLightPosition;
layout(location = 3) in vec4 inSurfacePosition;
layout(location = 4) in vec2 inTexCoord;

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
	vec3 lit = albedo * (lighting.Ambient.rgb + vec3(lambert * shadow + bounce));

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
