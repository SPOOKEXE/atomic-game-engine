#version 450

// Instanced opaque geometry: slot 0 is the mesh, slot 1 is per-instance data.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;

layout(location = 3) in vec4 inModel0;
layout(location = 4) in vec4 inModel1;
layout(location = 5) in vec4 inModel2;
layout(location = 6) in vec4 inModel3;
layout(location = 7) in vec4 inColour;

// One over the square of each axis' scale. See the normal below.
layout(location = 8) in vec4 inInverseScaleSquared;

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

void main() {
	mat4 model = mat4(inModel0, inModel1, inModel2, inModel3);

	// The model includes scale, so inverse scale squared corrects its normals.
	outNormal = mat3(model) * (inNormal * inInverseScaleSquared.xyz);
	outColour = inColour;
	outTexCoord = inTexCoord;

	vec4 world = model * vec4(inPosition, 1.0);

	// Divide in the fragment to preserve perspective-correct interpolation.
	outLightPosition = frame.LightViewProjection * world;
	outSurfacePosition = frame.SurfaceViewProjection * world;

	gl_Position = frame.ViewProjection * world;
}
