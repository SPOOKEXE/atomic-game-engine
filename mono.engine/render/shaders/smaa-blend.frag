#version 450

// SMAA, pass two of three: how much of each neighbour to take.
//
// **The real SMAA looks the pattern up in a precomputed area texture.** That
// texture encodes, for every shape a local edge can take, the exact coverage of
// the triangle it implies — which is what makes SMAA sharper than a blur that
// found the same edges. It is a baked asset this engine does not carry, so this
// computes weights from the edge neighbourhood directly.
//
// What that costs is precision on long shallow edges, which is the case the
// area texture handles best. What it keeps is the shape of the algorithm: edges
// found, weights derived, one blend — so a chain wiring the three passes gets
// antialiasing rather than a placeholder, and swapping in the lookup later
// changes this file and nothing around it.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outWeights;

layout(set = 2, binding = 0) uniform sampler2D edgesImage;

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
	vec2 texel = pass.Target.zw;

	vec2 here = texture(edgesImage, inUv).rg;

	// Nothing to blend. The resolve pass tests exactly this and skips.
	if (dot(here, here) == 0.0) {
		outWeights = vec4(0.0);
		return;
	}

	// How far the edge runs either way, which is what decides the weight: a
	// long straight edge is a shallow angle and wants a strong blend, a single
	// pixel is a corner and wants almost none.
	float leftRun = 0.0;
	float rightRun = 0.0;
	float upRun = 0.0;
	float downRun = 0.0;

	for (int index = 1; index <= 4; index++) {
		float reach = float(index);
		leftRun += texture(edgesImage, inUv - vec2(texel.x * reach, 0.0)).g;
		rightRun += texture(edgesImage, inUv + vec2(texel.x * reach, 0.0)).g;
		upRun += texture(edgesImage, inUv - vec2(0.0, texel.y * reach)).r;
		downRun += texture(edgesImage, inUv + vec2(0.0, texel.y * reach)).r;
	}

	// **Capped at half.** A weight of one replaces the pixel with its
	// neighbour, which moves the edge rather than smoothing it.
	float horizontal = here.r * min((leftRun + rightRun) / 8.0, 0.5);
	float vertical = here.g * min((upRun + downRun) / 8.0, 0.5);

	// Left, top, right, bottom — the order the resolve pass reads them in.
	outWeights = vec4(horizontal, vertical, horizontal, vertical);
}
