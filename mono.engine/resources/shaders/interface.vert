#version 450

// One quad corner of a compiled `gui::DrawList`.
//
// **Positions arrive in canvas pixels and are turned into clip space here.**
// `render::InterfaceMesh` builds vertices in the coordinates the layout worked
// in — top-left origin, Y down — so the only thing that knows the target's size
// is this shader, and a panel drawn at a different resolution needs no rebuild
// of the mesh.

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColour;

layout(location = 0) out vec2 outUv;
layout(location = 1) out vec4 outColour;

// Vertex uniforms are set 1 for SPIR-V.
layout(set = 1, binding = 0) uniform Canvas {
	// The target, in pixels. One divide rather than a matrix: an interface is
	// an axis-aligned orthographic projection and nothing here rotates.
	vec2 Size;
} canvas;

void main() {
	outUv = inUv;
	outColour = inColour;

	const vec2 normalised = inPosition / canvas.Size;

	// SDL's GPU clip space is Y-up on every backend — its Vulkan path submits a
	// negative-height viewport to make that true — while a canvas has its origin
	// at the top left with Y growing downward. So Y is flipped here and X is
	// not, which is the one asymmetry in this file and the one everybody writes
	// symmetrically the first time.
	gl_Position = vec4(normalised.x * 2.0 - 1.0, 1.0 - normalised.y * 2.0, 0.0, 1.0);
}
