#version 450
#extension GL_GOOGLE_include_directive : require

// Writes nothing. The depth attachment is the output.
//
// **A fragment shader is still required**, even for a depth-only pass: SDL's
// GPU API takes both stages when it creates a graphics pipeline, and a null
// fragment stage is not one of the things it accepts. An empty `main` is what
// every backend compiles this to anyway.

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) flat in uint inAppearance;
layout(location = 3) flat in float inInstanceAlpha;

layout(set = 2, binding = 0) uniform sampler2D colourMap;

#include "seam-mask.glsl"

// The half-space this draw keeps, matching `opaque.frag`'s `SeamPlane`.
//
// The compact shadow block carries only clipping state. The colour sampler is
// needed for `AlphaMode::Transparency`; opaque fragments take neither texture sample nor
// cutoff branch.
layout(set = 3, binding = 0) uniform Shadow {
	vec4 Plane;
	vec4 SeamFirst;
	vec4 SeamSecond;
	vec4 SeamCentre;
	// x: submesh base alpha.
	vec4 Material;
	vec4 Flipbook;
} shadow;

void main() {
	if (!KeepSeamSample(inWorldPosition, shadow.Plane, shadow.SeamFirst, shadow.SeamSecond, shadow.SeamCentre)) {
		discard;
	}

	uint alphaMode = inAppearance & 0xFFu;
	if (alphaMode == 1u) {
		vec2 cellUv = fract(inTexCoord) * shadow.Flipbook.x + shadow.Flipbook.yz;
		float alpha = inInstanceAlpha * texture(colourMap, cellUv).a * shadow.Material.x;
		float cutoff = float((inAppearance >> 8u) & 0xFFu) / 255.0;
		if (alpha < cutoff) {
			discard;
		}
	}
}
