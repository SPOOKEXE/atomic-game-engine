#version 450

// A billboard, expanded from the vertex index rather than from a vertex buffer.
//
// **No quad geometry is bound at all.** Four corners of a unit square are two bit
// tests, and a vertex fetch is a memory access - at half a million particles the
// difference is half a million cache lines that are never read. `gl_VertexIndex`
// runs 0..3 and the draw is a triangle strip, so one instance is four vertices
// and no index buffer either.
//
// Slot 0 is the per-instance particle. There is no slot 1.

layout(location = 0) in vec3 inPosition;

// Width and height, each 16 bits over a 64-metre ceiling. Unpacked here rather
// than uploaded as floats, because four bytes across half a million particles is
// two megabytes a frame.
layout(location = 1) in uint inSize;

// Rotation in the low 16 bits as a turn over 65536; flipbook cell in the high 16.
layout(location = 2) in uint inRotationAndCell;

layout(location = 3) in uint inColour;

// Which emitter, so the fragment stage can pick its texture rectangle. Passed
// through rather than used here.
layout(location = 4) in uint inSlot;

layout(set = 1, binding = 0) uniform Frame {
	mat4 ViewProjection;

	// The camera's own axes in world space, so a facing quad is two adds rather
	// than an inverse per particle.
	vec4 CameraRight;
	vec4 CameraUp;
	vec4 CameraForward;

	// x: how many cells the flipbook has on each side, as a float so the divide
	// is one multiply. y: how far towards the camera to nudge. z: whether the
	// quad keeps world up. w: unused, named so the struct's size is stated.
	vec4 Options;
} frame;

layout(location = 0) out vec2 outTexCoord;
layout(location = 1) out vec4 outColour;

void main() {
	// **The corner comes out of the index, not out of a buffer.** Bit 0 is the
	// right edge and bit 1 is the top, which walks 0,1,2,3 as bottom-left,
	// bottom-right, top-left, top-right - the winding a triangle strip wants.
	vec2 corner = vec2(
		(gl_VertexIndex & 1) == 0 ? -0.5 : 0.5,
		(gl_VertexIndex & 2) == 0 ? -0.5 : 0.5
	);

	// 64 metres over 65535, matching `PackParticleSize`. Spelled as the constant
	// rather than passed in, because a mismatch between the two would be a scene
	// where every particle is the wrong size by a fixed factor - which reads as
	// an authoring mistake rather than as a shader one.
	const float SIZE_SCALE = 64.0 / 65535.0;
	vec2 size = vec2(float(inSize & 0xFFFFu), float(inSize >> 16)) * SIZE_SCALE;

	float turn = float(inRotationAndCell & 0xFFFFu) * (6.2831853 / 65536.0);
	float sinTurn = sin(turn);
	float cosTurn = cos(turn);

	vec2 spun = vec2(
		corner.x * cosTurn - corner.y * sinTurn,
		corner.x * sinTurn + corner.y * cosTurn
	) * size;

	// World up rather than the camera's up when asked, so a column of smoke does
	// not roll when the camera does.
	vec3 up = frame.Options.z > 0.5 ? vec3(0.0, 1.0, 0.0) : frame.CameraUp.xyz;
	vec3 right = normalize(cross(up, frame.CameraForward.xyz));
	vec3 quadUp = cross(frame.CameraForward.xyz, right);

	// The Z offset moves the particle towards the eye in *world* space, so it
	// sorts in front of the geometry it is attached to without being moved where
	// its physics happen.
	vec3 world = inPosition
		+ right * spun.x
		+ quadUp * spun.y
		- frame.CameraForward.xyz * frame.Options.y;

	// The flipbook cell picks a rectangle of the texture. A side of 1 - no
	// flipbook - makes this the whole of it, with no branch.
	float side = frame.Options.x;
	uint cell = inRotationAndCell >> 16;
	vec2 cellOrigin = vec2(float(cell % uint(side)), float(cell / uint(side))) / side;

	outTexCoord = cellOrigin + (corner + 0.5) / side;

	// RGBA8 in one word, little-endian in the order the packer wrote it.
	outColour = vec4(
		float(inColour & 0xFFu),
		float((inColour >> 8) & 0xFFu),
		float((inColour >> 16) & 0xFFu),
		float(inColour >> 24)
	) * (1.0 / 255.0);

	gl_Position = frame.ViewProjection * vec4(world, 1.0);
}
