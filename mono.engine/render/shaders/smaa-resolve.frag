#version 450

// SMAA, pass three of three: the blend the weights describe.
//
// **One bilinear tap per direction rather than one per neighbour.** Offsetting
// the sample position by the weight makes the hardware do the interpolation, so
// a weighted average of two pixels costs one fetch. That trick is why the pass
// is nearly free, and it is the reason the weights are a fraction of a texel
// rather than a blend factor.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D colourImage;
layout(set = 2, binding = 1) uniform sampler2D weightsImage;

// What this pass is shading. **Every raster pass is given this block**, a
// shipped effect and one an author writes alike — see `PassUniforms` in
// `Renderer.cpp`. A shader that does not declare it is not penalised: the
// pipeline reserves the slot either way and an unused buffer costs nothing,
// which is what keeps `tint.frag` and every custom shader written before this
// existed working unchanged.
layout(set = 3, binding = 0) uniform Pass {
	mat4 ViewProjection;
	mat4 InverseViewProjection;
	vec4 Planes; // near, far, 1/near, 1/far
	vec4 Target; // width, height, 1/width, 1/height
	vec4 View;   // seconds, vertical field of view, aspect, unused
}
pass;

void main() {
	vec4 weights = texture(weightsImage, inUv);

	// **Untouched where there was no edge**, which is most of the frame. This
	// is the early out pass one's threshold exists to produce.
	if (dot(weights, weights) == 0.0) {
		outColour = texture(colourImage, inUv);
		return;
	}

	vec2 texel = pass.Target.zw;

	// The stronger of each opposing pair decides which way to shift, so a pixel
	// with an edge on both sides moves towards the one that dominates instead
	// of cancelling to nothing.
	float horizontal = weights.z - weights.x;
	float vertical = weights.w - weights.y;

	vec2 offset = vec2(horizontal * texel.x, vertical * texel.y);
	outColour = texture(colourImage, inUv + offset);
}
