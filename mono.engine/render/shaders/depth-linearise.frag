#version 450

// Depth as distance, rather than as whatever the projection left in the buffer.
//
// **This is the pass that could not be written before raster passes had
// uniforms.** A depth buffer is non-linear by construction — a perspective
// divide puts most of its precision near the eye — and turning it back into
// metres needs the near and far planes. Without them a shader can only guess,
// and every consumer downstream inherits the guess.
//
// The catalogue's other depth readers all want this rather than the raw buffer:
// `ssao`, `dof` and `depth-discontinuity` each declare `R32` linear depth as
// their input, which is exactly what this writes.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outLinear;

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

void main() {
	float raw = texture(depthImage, inUv).r;

	// **The far plane stays at the far plane.** Reversing the divide at raw
	// depth 1 is a division by zero on an infinite projection, and clamping the
	// result afterwards turns the sky into whatever the arithmetic produced
	// rather than into "as far as this camera sees".
	if (raw >= 1.0) {
		outLinear = vec4(pass.Planes.y);
		return;
	}

	// Standard reversal of a finite perspective projection: near*far divided by
	// far minus depth times (far-near). Written with the planes rather than with
	// a packed pair of constants so it reads as the formula it is.
	float near = pass.Planes.x;
	float far = pass.Planes.y;
	outLinear = vec4((near * far) / max(far - raw * (far - near), 1e-6));
}
