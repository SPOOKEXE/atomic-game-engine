#version 450
#extension GL_GOOGLE_include_directive : require

// The ground grid, drawn as a plane rather than as lines.
//
// **A fullscreen triangle that finds the ground for itself.** Every pixel casts
// its own ray through the inverse view-projection, intersects `y = 0`, and asks
// whether that world point is on a grid line. There is no vertex buffer, no
// line list, and nothing to upload when the camera moves - which is the whole
// reason the grid can be depth-tested at all. The editor's grid was an imgui
// overlay drawn after the world, and an overlay has no depth buffer to test
// against, so it drew over the geometry it should have been under.
//
// **`gl_FragDepth` is what makes the hardware do the occluding.** The plane's
// own clip depth is written per pixel, so the depth test the pipeline was
// created with compares it against whatever the opaque pass left - a wall in
// front of the grid hides it, exactly as a wall hides anything else. Nothing is
// sampled and nothing is compared by hand.
//
// **The line width is a screen-space width, from `fwidth`.** A grid drawn as
// world-space quads is a pixel wide near the camera and a fifth of one at the
// horizon, which aliases into a shimmering mess. Dividing the distance to the
// nearest line by its own screen derivative gives a line that is the same
// weight everywhere and fades out on its own when the cells become smaller than
// a pixel - which is the analytic form of a mip chain and needs no texture.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

#include "grid.glsl"

void main() {
	vec4 colour;
	float depth;
	if (!EvaluateGroundGrid(inUv, colour, depth)) discard;
	gl_FragDepth = depth;
	outColour = colour;
}
