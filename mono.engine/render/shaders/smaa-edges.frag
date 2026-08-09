#version 450

// SMAA, pass one of three: where the image has edges.
//
// **Luma rather than colour.** Two colours of equal brightness meeting is not
// an edge the eye reads as a jagged step, and treating it as one puts the
// blend weights of the next pass onto boundaries that never aliased.
//
// **Left and top neighbours only, not all four.** Every pixel runs this, so the
// edge between two pixels is found once from one side rather than twice from
// both — the same reason the reference implementation does it. The next pass
// reads the neighbour's own result to see the other half.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outEdges;

layout(set = 2, binding = 0) uniform sampler2D colourImage;

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

const float THRESHOLD = 0.1;

float Luma(vec3 colour) {
	return dot(colour, vec3(0.2126, 0.7152, 0.0722));
}

void main() {
	vec2 texel = pass.Target.zw;

	float centre = Luma(texture(colourImage, inUv).rgb);
	float left = Luma(texture(colourImage, inUv - vec2(texel.x, 0.0)).rgb);
	float top = Luma(texture(colourImage, inUv - vec2(0.0, texel.y)).rgb);

	vec2 delta = vec2(abs(centre - left), abs(centre - top));
	vec2 edges = step(vec2(THRESHOLD), delta);

	// **Both zero means nothing here**, and saying so explicitly matters: the
	// resolve pass skips a pixel with no edges entirely, and that early out is
	// most of why SMAA is affordable.
	outEdges = vec4(edges, 0.0, 1.0);
}
