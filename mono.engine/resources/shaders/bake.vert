#version 450
#extension GL_GOOGLE_include_directive : require

// A mesh laid out over its own UVs, for baking a surface into textures. The
// surface reads it as a rest-pose part at the origin: white, unscaled, with its
// editable channels.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4 inWeights;

// Bindings 0 to 3, which SDL numbers by counting, so binding 4 lands where the
// draw binds the editable channels.
#include "instance.glsl"
#include "editable-attributes.glsl"

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec4 outColour;
layout(location = 2) out vec4 outLightPosition;
layout(location = 3) out vec4 outSurfacePosition;
layout(location = 4) out vec2 outTexCoord;
layout(location = 5) out vec3 outWorldPosition;
layout(location = 6) flat out uint outAppearance;
layout(location = 7) flat out vec3 outSurfaceColour;
layout(location = 8) flat out vec4 outEmission;
layout(location = 9) flat out uvec2 outFeaturePolicy;
layout(location = 10) flat out uvec3 outLabels;
layout(location = 11) out vec4 outAttribute0;
layout(location = 12) out vec4 outAttribute1;
layout(location = 13) out vec4 outAttribute2;
layout(location = 14) out vec4 outAttribute3;

void main() {
	outNormal = normalize(inNormal);
	outColour = vec4(1.0);
	// Never taken; it keeps the instance storage declared.
	if (gl_VertexIndex < 0) outColour = InstanceColour(LoadInstance());
	outLightPosition = vec4(0.0, 0.0, 0.0, 1.0);
	outSurfacePosition = vec4(0.0, 0.0, 0.0, 1.0);
	outTexCoord = inTexCoord;
	outWorldPosition = inPosition;
	outAppearance = 0u;
	outSurfaceColour = vec3(1.0);
	outEmission = vec4(0.0);
	outFeaturePolicy = uvec2(0u);
	outLabels = uvec3(0u);
	vec4 channels[4];
	LoadEditableAttributes(uint(gl_VertexIndex), channels);
	outAttribute0 = channels[0];
	outAttribute1 = channels[1];
	outAttribute2 = channels[2];
	outAttribute3 = channels[3];
	// The first UDIM tile, with UV 0,0 at the image's top left.
	gl_Position = vec4(inTexCoord.x * 2.0 - 1.0, 1.0 - inTexCoord.y * 2.0, 0.5, 1.0);
}
