#version 450

// The engine's cel shader: the same light, quantised into bands.
//
// **Reached by name**, exactly as `unlit.frag` is: `render::BuiltInShaderNames`
// lists `toon`, and `Material.Shader = "toon"` is what loads this. See that
// file's header for why the list and this directory are added to together.
//
// **The second default, and the reason there are two rather than six.** One
// shader proves a name resolves; two prove the *selection* does — a scene with
// an unlit sign and a toon character draws two pipelines from one frame, which
// is the thing `Renderer::AddShader` and the per-run pipeline bind in
// `DrawSlots` exist to do. A third would prove nothing further and would be a
// permutation somebody added by hand, which is what the render graph is for.
//
// ## The interface is `opaque.frag`'s
//
// The `Lighting` block is copied whole for the reason `unlit.frag` gives: it is
// pushed as one blob, so a member left out moves every member after it.
//
// **What this deliberately does not do is sample the shadow map.** A cel shader
// with a four-tap percentage-closer filter has a soft shadow edge inside a hard
// band, which reads as the bands being broken rather than as a soft shadow. A
// toon shadow is its own decision — a hard tap against the same map — and it
// belongs to whoever wants it, in a `ShaderScript`, over this file as a
// starting point.

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColour;
layout(location = 2) in vec4 inLightPosition;
layout(location = 3) in vec4 inSurfacePosition;
layout(location = 4) in vec2 inTexCoord;
layout(location = 5) in vec3 inWorldPosition;

layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D shadowMap;
layout(set = 2, binding = 1) uniform sampler2D surfaceMap;
layout(set = 2, binding = 2) uniform sampler2D colourMap;
layout(set = 2, binding = 3) uniform sampler2D beamMap;

layout(set = 3, binding = 0) uniform Lighting {
	vec4 Direction;
	vec4 Ambient;
	vec4 Flags;
	vec4 BaseColour;
	vec4 Surface;
	vec4 Flipbook;
	vec4 Mirror;
	vec4 PaneNormal;
	vec4 SeamPlane;
} lighting;

// How many steps the diffuse term is cut into.
//
// **Three, which is the fewest that reads as a drawing rather than as a
// posterised photograph.** Two gives a hard terminator and no form; four starts
// to look like a gradient with a bug in it. The classic cel look is a lit band,
// a mid band and a shadow band.
#define TOON_BANDS 3.0

void main() {
	const vec2 cellUv = fract(inTexCoord) * lighting.Flipbook.x + lighting.Flipbook.yz;

	vec4 sampled = lighting.Surface.x > 0.5 ? texture(colourMap, cellUv) : vec4(1.0);

	// The cut-out and the seam, kept for `unlit.frag`'s reason: a scene must not
	// break by having a shader selected in it.
	float alpha = inColour.a * sampled.a * lighting.BaseColour.a;
	if (lighting.Surface.y > 0.0 && alpha < lighting.Surface.y) {
		discard;
	}

	if (dot(lighting.SeamPlane.xyz, lighting.SeamPlane.xyz) > 0.0 &&
		dot(inWorldPosition, lighting.SeamPlane.xyz) < lighting.SeamPlane.w) {
		discard;
	}

	vec3 albedo = inColour.rgb * sampled.rgb * lighting.BaseColour.rgb;

	vec3 normal = normalize(inNormal);
	vec3 toLight = -normalize(lighting.Direction.xyz);

	// **Quantised before the ambient is added, not after.** Banding the final
	// colour would put a step in the shadow side as well, where there is no
	// light to step — and the ambient is what keeps the darkest band from being
	// black.
	float lambert = max(dot(normal, toLight), 0.0);
	float banded = floor(lambert * TOON_BANDS + 0.5) / TOON_BANDS;

	// A rim light along the silhouette, which is the other half of what makes
	// this read as cel shading: the bands give the form and the rim gives the
	// outline. Cheap — it is the same normal against the same eye direction the
	// rasteriser already interpolated.
	//
	// **Derived from the world position rather than from a camera uniform**,
	// because this block carries no eye position and adding one would change a
	// struct every pass pushes. `dFdx`/`dFdy` of the world position give the
	// surface's own screen-space frame, whose cross product is the facing
	// direction — one that is right whichever camera is looking, including the
	// several a mirror renders through.
	vec3 facing = normalize(cross(dFdx(inWorldPosition), dFdy(inWorldPosition)));
	float rim = 1.0 - abs(dot(normal, facing));
	float edge = smoothstep(0.62, 0.92, rim);

	vec3 lit = albedo * (lighting.Ambient.rgb + vec3(banded));
	outColour = vec4(mix(lit, albedo, edge), alpha);
}
