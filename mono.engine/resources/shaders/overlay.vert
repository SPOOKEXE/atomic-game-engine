#version 450

// A fullscreen triangle from three vertex indices and no vertex buffer. One
// triangle rather than two: the diagonal seam of a quad costs a second
// rasterisation of every pixel along it.

layout(location = 0) out vec2 outUv;

void main() {
	vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	outUv = uv;

	// SDL's GPU clip space is Y-up on every backend - its Vulkan path submits
	// a negative-height viewport to make that true. Row 0 of the overlay image
	// is the top of the screen, so v of 0 has to become +1 rather than -1.
	gl_Position = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}
