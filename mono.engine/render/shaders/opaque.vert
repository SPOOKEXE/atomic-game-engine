#version 450

// Instanced opaque geometry. Slot 0 is the mesh, slot 1 is one entry per
// instance — a model matrix and a colour. The mesh is uploaded once; only the
// instance buffer is rewritten each frame.

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
layout(location = 4) out vec2 outTexCoord;

void main() {
	mat4 model = mat4(inModel0, inModel1, inModel2, inModel3);

	// **The model matrix is not rigid — a half-extent is folded into it — so
	// the upper 3x3 is not the normal matrix.** For `M = R * S` the normal
	// transform is `R * inverse(S)`, and since `mat3(M) * v` is `R * S * v`,
	// scaling the normal by one over S squared first gets there: `R * S * (S^-2
	// n)` is `R * S^-1 n`. That is three multiplies rather than an inverse
	// transpose per vertex.
	//
	// It cost nothing while everything was a cube — an axis-aligned normal
	// comes out of the wrong matrix pointing the right way and is renormalised
	// in the fragment — and it is visibly wrong the moment a sphere or an
	// imported mesh is scaled unevenly, where the shading tilts away from the
	// surface.
	outNormal = mat3(model) * (inNormal * inInverseScaleSquared.xyz);
	outColour = inColour;
	outTexCoord = inTexCoord;

	vec4 world = model * vec4(inPosition, 1.0);

	// **Interpolated in clip space and divided in the fragment**, not divided
	// here. Dividing per vertex and interpolating the result is affine where
	// the projection is perspective, and the error shows as shadows that slide
	// across a large triangle as the camera moves.
	outLightPosition = frame.LightViewProjection * world;
	outSurfacePosition = frame.SurfaceViewProjection * world;

	gl_Position = frame.ViewProjection * world;
}
