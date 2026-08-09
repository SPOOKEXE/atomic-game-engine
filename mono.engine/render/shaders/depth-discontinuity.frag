#version 450

// Where the depth buffer steps: silhouettes, and the creases between surfaces.
//
// **Takes linear depth, which is why `depth-linearise` exists.** A step in raw
// depth means something completely different at two metres and at two hundred,
// so a threshold over the raw buffer finds every edge near the camera and none
// far from it. Over metres one threshold means one thing everywhere.
//
// **A relative threshold rather than an absolute one.** A 10cm step matters on
// a doorframe and is nothing on a distant hillside; dividing by the centre
// distance is what makes a single number work across a scene.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outEdges;

layout(set = 2, binding = 0) uniform sampler2D depthImage;

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

const float THRESHOLD = 0.02;

void main() {
	vec2 texel = pass.Target.zw;

	float centre = texture(depthImage, inUv).r;
	float left = texture(depthImage, inUv - vec2(texel.x, 0.0)).r;
	float right = texture(depthImage, inUv + vec2(texel.x, 0.0)).r;
	float down = texture(depthImage, inUv - vec2(0.0, texel.y)).r;
	float up = texture(depthImage, inUv + vec2(0.0, texel.y)).r;

	// The larger of the two one-sided differences per axis, not the centred
	// one: a centred difference is zero in the middle of a ramp *and* at the
	// exact centre of a symmetric step, which is the one place an edge detector
	// must not report nothing.
	float horizontal = max(abs(centre - left), abs(centre - right));
	float vertical = max(abs(centre - down), abs(centre - up));

	float relative = max(horizontal, vertical) / max(centre, 1e-4);
	float edge = step(THRESHOLD, relative);

	// The strength beside the flag, because a consumer that wants to weight by
	// how sharp the crease is should not have to recompute it.
	outEdges = vec4(edge, clamp(relative, 0.0, 1.0), 0.0, 1.0);
}
