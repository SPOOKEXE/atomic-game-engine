#version 450

// Instanced opaque geometry. Slot 0 is the mesh, slot 1 is one entry per
// instance — a model matrix and a colour. The mesh is uploaded once; only the
// instance buffer is rewritten each frame.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 2) in vec4 inModel0;
layout(location = 3) in vec4 inModel1;
layout(location = 4) in vec4 inModel2;
layout(location = 5) in vec4 inModel3;
layout(location = 6) in vec4 inColour;

// SDL's GPU API puts vertex uniform buffers in set 1 for SPIR-V.
layout(set = 1, binding = 0) uniform Frame {
	mat4 ViewProjection;

	// World space to the light's clip space, for the shadow lookup. The same
	// matrix the depth pass rendered with — passed rather than recomputed,
	// because a second derivation is a second chance to disagree and the
	// symptom would be shadows offset from what casts them.
	mat4 LightViewProjection;

	// Where the surface camera is looking, for the planar projection a mirror
	// samples with. Identity when nothing renders to a surface.
	mat4 SurfaceViewProjection;
} frame;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec4 outColour;
layout(location = 2) out vec4 outLightPosition;
layout(location = 3) out vec4 outSurfacePosition;

void main() {
	mat4 model = mat4(inModel0, inModel1, inModel2, inModel3);

	// The transforms are rigid — rotation and translation, no scale — so the
	// upper 3x3 is orthonormal and doubles as the normal matrix. An inverse
	// transpose here would be three wasted matrix operations per vertex.
	outNormal = mat3(model) * inNormal;
	outColour = inColour;

	vec4 world = model * vec4(inPosition, 1.0);

	// **Interpolated in clip space and divided in the fragment**, not divided
	// here. Dividing per vertex and interpolating the result is affine where
	// the projection is perspective, and the error shows as shadows that slide
	// across a large triangle as the camera moves.
	outLightPosition = frame.LightViewProjection * world;
	outSurfacePosition = frame.SurfaceViewProjection * world;

	gl_Position = frame.ViewProjection * world;
}
