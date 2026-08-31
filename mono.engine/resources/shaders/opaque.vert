#version 450
#extension GL_GOOGLE_include_directive : require

// Instanced opaque geometry: attributes are the mesh; storage holds resident
// instance rows and this view's ordered indices.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4 inWeights;

// The resident-row storage declarations and their decode.
#include "instance.glsl"

// SDL's GPU API puts vertex uniform buffers in set 1 for SPIR-V.
layout(set = 1, binding = 0) uniform Frame {
	mat4 ViewProjection;

	// World space to the light's clip space; matches the depth pass.
	mat4 LightViewProjection;

	// Surface-camera projection, or identity for ordinary geometry.
	mat4 SurfaceViewProjection;
} frame;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec4 outColour;
layout(location = 2) out vec4 outLightPosition;
layout(location = 3) out vec4 outSurfacePosition;
layout(location = 4) out vec2 outTexCoord;

// Where this fragment is, for the point and spot lights' falloff. **The world
// position and not the view one**, because a light's range is in metres and a
// view-space distance would make it depend on where the camera is.
layout(location = 5) out vec3 outWorldPosition;
layout(location = 6) flat out uint outAppearance;
layout(location = 7) flat out vec3 outSurfaceColour;
layout(location = 8) flat out vec4 outEmission;

void main() {
	// **No model matrix is built.** The instance row carries the rotation, the
	// scale and the translation separately, so the position is one quaternion
	// rotate and an add - and the normal is the same rotate against a reciprocal
	// scale, which is what the old `inInverseScaleSquared` float4 was for.
	InstanceRow instance = LoadInstance();
	vec3 position = InstancePosition(instance);
	vec3 scale = InstanceScale(instance);
	vec4 rotation = InstanceRotation(instance);
	vec3 meshPosition = inPosition;
	vec3 meshNormal = inNormal;
	ApplySkin(inJoints, inWeights, meshPosition, meshNormal);

	outNormal = InstanceWorldNormal(rotation, scale, meshNormal);
	outColour = InstanceColour(instance);
	outTexCoord = inTexCoord;
	outAppearance = InstanceAppearance(instance);
	outSurfaceColour = InstanceSurfaceColour(instance);
	outEmission = InstanceEmission(instance);

	vec4 world = vec4(InstanceWorldPosition(rotation, scale, position, meshPosition), 1.0);
	outWorldPosition = world.xyz;

	// Divide in the fragment to preserve perspective-correct interpolation.
	outLightPosition = frame.LightViewProjection * world;
	outSurfacePosition = frame.SurfaceViewProjection * world;

	gl_Position = frame.ViewProjection * world;
}
