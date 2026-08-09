#version 450

// A custom fullscreen pass, and the smallest one that proves the path.
//
// **What this file is for is the wiring, not the effect.** A `raster` node names
// a fragment shader by parameter; the renderer loads it, builds a pipeline
// against the target's format, binds what the node reads as samplers, and draws
// three vertices. Nothing about that is specific to this shader — this one is
// here so the path has something real to run, and so a pipeline somebody authors
// can be checked against a picture rather than against a hope.
//
// **The vertex stage is `overlay.vert` unchanged**, which already builds a
// fullscreen triangle from three indices and hands out a UV. Every custom raster
// pass shares it, so where a fullscreen pass puts its vertices is described once.
//
// Sepia rather than a plain multiply: a tint that only scaled channels would
// look the same as a texture that happened to be dark, and the point of a proof
// is that its output could not be anything else.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D sourceImage;

void main() {
	vec4 source = texture(sourceImage, inUv);

	float luminance = dot(source.rgb, vec3(0.299, 0.587, 0.114));
	outColour = vec4(
		clamp(luminance * 1.07, 0.0, 1.0),
		clamp(luminance * 0.74, 0.0, 1.0),
		clamp(luminance * 0.43, 0.0, 1.0),
		source.a
	);
}
