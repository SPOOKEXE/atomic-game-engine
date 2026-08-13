#version 450

// One ribbon vertex, already placed in world space by `effects::BuildRibbons`.
//
// **Almost nothing happens here, and that is the design.** A beam's curve, its
// width taper and its camera-facing turn are all resolved on the CPU, in
// `shared`, where they can be wrong in a test rather than on somebody's screen —
// `scene::OrderScene`'s argument, applied to a ribbon. What is left for the
// vertex stage is a matrix multiply and an unpack.
//
// The alternative was a shader that took two endpoints and expanded the strip
// itself. It would move the work to where it is cheaper and put the geometry
// somewhere no suite can reach, and a beam that renders in the wrong plane is
// exactly the kind of bug that is invisible in a screenshot taken from the one
// angle somebody tried.

layout(location = 0) in vec3 inPosition;

// How far along the ribbon, and which side of the strip. Both are texture
// coordinates, which is why they are one attribute.
layout(location = 1) in vec2 inCoordinate;

layout(location = 2) in uint inColour;

layout(set = 1, binding = 0) uniform Frame {
	mat4 ViewProjection;

	// The eye's forward, for the Z offset. x, y, z used; w unused and named so
	// the struct's size is stated rather than implied.
	vec4 CameraForward;

	// x: how far towards the eye to nudge, in metres. The rest is named for
	// `CameraForward`'s reason.
	vec4 Options;
} frame;

layout(location = 0) out vec2 outTexCoord;
layout(location = 1) out vec4 outColour;

void main() {
	outTexCoord = inCoordinate;

	// RGBA8 in one word, in the order `PackRgba` wrote it.
	outColour = vec4(
		float(inColour & 0xFFu),
		float((inColour >> 8) & 0xFFu),
		float((inColour >> 16) & 0xFFu),
		float(inColour >> 24)
	) * (1.0 / 255.0);

	// The offset moves the ribbon towards the eye in world space, so it sorts in
	// front of the geometry it is attached to without being moved where its
	// endpoints are.
	vec3 world = inPosition - frame.CameraForward.xyz * frame.Options.x;
	gl_Position = frame.ViewProjection * vec4(world, 1.0);
}
