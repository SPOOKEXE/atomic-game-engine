#version 450

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColour;
layout(location = 2) in vec4 inLightPosition;
layout(location = 3) in vec4 inSurfacePosition;

layout(location = 0) out vec4 outColour;

// Fragment samplers are set 2 for SPIR-V; uniform buffers are set 3.
layout(set = 2, binding = 0) uniform sampler2D shadowMap;
layout(set = 2, binding = 1) uniform sampler2D surfaceMap;

layout(set = 3, binding = 0) uniform Lighting {
	vec4 Direction;
	vec4 Ambient;

	// x: 1 when a shadow map was rendered this frame, 0 otherwise.
	// y: one texel of the shadow map, for the sample offsets.
	// z: 1 when this draw samples the surface texture rather than its own tint.
	// w: how opaque the projected image is, 0 to 1. See the composite below.
	vec4 Flags;
} lighting;

// How much light reaches this fragment, 0 fully shadowed to 1 fully lit.
float ShadowFactor(vec3 normal, vec3 toLight) {
	if (lighting.Flags.x < 0.5) {
		return 1.0;
	}

	// The perspective divide, done here rather than in the vertex shader: see
	// `opaque.vert`. For the orthographic light matrix `w` is one and this
	// costs nothing, but doing it correctly means the projection can change
	// without this becoming subtly wrong.
	vec3 projected = inLightPosition.xyz / inLightPosition.w;

	// **Y is flipped, and that is SDL's viewport rather than a convention
	// mistake.** SDL's Vulkan backend submits a negative-height viewport "for
	// consistency with other backends", so a vertex at `ndc.y = +1` lands at the
	// *top* of the target — where a texture's `v = 0` is. Sampling with
	// `ndc.y * 0.5 + 0.5` reads the image upside down.
	//
	// Depth needs no remap: Vulkan clip space is already 0..1 there, and an
	// OpenGL-convention rescale of z would push every comparison half a unit and
	// shadow the whole scene.
	vec2 uv = vec2(projected.x * 0.5 + 0.5, 0.5 - projected.y * 0.5);

	// Outside the map is lit, not shadowed. The map covers what the scene
	// reaches; a fragment past its edge is one the light was never fitted to,
	// and shadowing it would put a hard black border around the world.
	if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || projected.z > 1.0) {
		return 1.0;
	}

	// **A slope-scaled bias, not a constant one.** A surface nearly edge-on to
	// the light spans many depth values within one shadow texel, so a constant
	// bias large enough to stop it self-shadowing is large enough to detach
	// shadows from their casters everywhere else. Scaling by the angle pays the
	// cost only where it is needed.
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

	vec3 albedo = inColour.rgb;

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

			// **Composited over the part rather than replacing it, and the
			// alphas are combined rather than one winning.** This is the fix for
			// a mirror that vanished when it was faded: the image used to be
			// written with the *part's* alpha, so a pane at `Transparency = 0.9`
			// drew its reflection at one tenth strength and a fully transparent
			// pane showed no reflection at all — which is not what glass does.
			//
			// The two are independent facts. `inColour.a` is how much of the
			// world behind shows through the pane; `imageAlpha` is how much of
			// the pane shows through the reflection. Taking the max means the
			// image is as solid as it was authored to be **whatever the part's
			// own transparency is**, so a reflection on invisible glass is a
			// floating reflection — which is exactly what a mirror is.
			//
			// The colour is mixed in the same proportion, so a half-opaque image
			// on a tinted pane is half of each rather than one or the other.
			outColour = vec4(mix(lit, image, imageAlpha), max(inColour.a, imageAlpha));
			return;
		}
	}

	outColour = vec4(lit, inColour.a);
}
