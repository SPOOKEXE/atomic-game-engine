#version 450

// The engine's flat shader: albedo, and nothing done to it.
//
// **Reached by name.** `render::BuiltInShaderNames` lists `unlit`, a
// `Material.Shader = "unlit"` resolves to this file, and `Renderer::AddShader`
// builds the pipelines that draw through it. `docs/DEFERRED.md` D00110 is the
// entry that says why that order matters: a `.frag` in this directory that no
// name resolves to would compile, stage, pass every test and be loaded by
// nothing, and `resources/AGENTS.md` refuses exactly that.
//
// **What it is for.** A billboard, an emissive sign, a debug marker, a UI panel
// in the world - anything whose colour is the answer rather than something the
// sun has an opinion about. It is also the smallest complete example of what a
// `ShaderScript` may replace: an author copies this file, changes the last line
// and has a working shader.
//
// ## The interface, which is `opaque.frag`'s and is not negotiable
//
// `Renderer::AddShader` creates every fragment shader with ten samplers and
// three uniform buffers, because a shader object carries those counts rather
// than the pipeline doing so. A module declaring a different arrangement binds
// and silently samples nothing.
//
// **So the `Lighting` block below is copied whole, including the members this
// file never reads.** It is pushed as one blob at set 3 binding 0, so a member
// left out here does not shrink the struct - it moves every member after it,
// and a shader reading `BaseColour` out of `Flags`' bytes produces a colour
// nobody can explain. The fields are documented in `opaque.frag`; this copy
// stays in step with that one.

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColour;
layout(location = 2) in vec4 inLightPosition;
layout(location = 3) in vec4 inSurfacePosition;
layout(location = 4) in vec2 inTexCoord;
layout(location = 5) in vec3 inWorldPosition;
layout(location = 6) flat in uint inAppearance;
layout(location = 7) flat in vec3 inSurfaceColour;
layout(location = 8) flat in vec4 inEmission;

layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D shadowMap;
layout(set = 2, binding = 1) uniform sampler2D surfaceMap;
layout(set = 2, binding = 2) uniform sampler2D colourMap;
layout(set = 2, binding = 3) uniform sampler2D beamMap;

layout(set = 3, binding = 0) uniform Lighting {
	vec4 Direction;
	vec4 Ambient;
	vec4 Direct;
	vec4 Flags;
	vec4 BaseColour;
	vec4 Surface;
	vec4 Material;
	vec4 Flipbook;
	vec4 Mirror;
	vec4 PaneNormal;
	vec4 SeamPlane;
	vec4 OutdoorAmbient;
	vec4 FogColour;
	vec4 Fog;
	vec4 Eye;
	vec4 MaterialExtra;
} lighting;

float FogFactor() {
	float interval = max(lighting.Fog.y - lighting.Fog.x, 0.0001);
	return clamp((distance(inWorldPosition, lighting.Eye.xyz) - lighting.Fog.x) / interval, 0.0, 1.0);
}

void main() {
	// `fract` before the cell transform, so a tiled coordinate stays inside its
	// cell - `opaque.frag` records what happens without it.
	const vec2 cellUv = fract(inTexCoord) * lighting.Flipbook.x + lighting.Flipbook.yz;

	vec4 sampled = lighting.Surface.x > 0.5 ? texture(colourMap, cellUv) : vec4(1.0);

	// **The cut-out and the seam are kept, and they are not decoration.** A
	// shader that dropped them would draw hair cards as solid rectangles and
	// would draw both halves of a body standing in a portal whole - so a scene
	// would break by selecting a shader, which is the one thing selecting one
	// must not do. Every default here keeps them, and a `ShaderScript` that
	// wants to work in those scenes keeps them too.
	uint alphaMode = inAppearance & 0xFFu;
	float cutoff = float((inAppearance >> 8u) & 0xFFu) / 255.0;
	float materialAlpha = sampled.a * lighting.BaseColour.a;
	if (alphaMode == 1u && inColour.a >= 0.98 && materialAlpha < cutoff) {
		discard;
	}
	float alpha = alphaMode == 1u ? inColour.a * materialAlpha : inColour.a;

	if (dot(lighting.SeamPlane.xyz, lighting.SeamPlane.xyz) > 0.0 &&
		dot(inWorldPosition, lighting.SeamPlane.xyz) < lighting.SeamPlane.w) {
		discard;
	}

	// The three colour sources multiply rather than one winning, which is
	// `opaque.frag`'s rule and the reason an untextured import looks right: the
	// texture is what was painted, the base colour is what the material says
	// the run is, and the tint is what the scene says this copy is.
	vec3 mappedColour = sampled.rgb * lighting.BaseColour.rgb;
	vec3 colour = inColour.rgb * mappedColour * inSurfaceColour;
	if (alphaMode == 0u) {
		colour = mix(inColour.rgb, colour, materialAlpha);
	} else if (alphaMode == 2u) {
		colour = inColour.rgb * mappedColour * mix(vec3(1.0), inSurfaceColour, materialAlpha);
	}
	outColour = vec4(mix(colour, lighting.FogColour.rgb, FogFactor()), alpha);
}
