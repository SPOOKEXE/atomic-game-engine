#version 450

// Depth of field: what the lens is not focused on goes soft.
//
// **Takes linear depth, so the circle of confusion is in metres.** Focus is a
// distance — "sharp at four metres" — and expressing that against a raw depth
// buffer means a focus plane that moves when the far plane does. `R32` linear
// is what the catalogue declares and what `depth-linearise` writes.
//
// **A fixed focus distance rather than one read from the scene.** Autofocus is
// a decision about what the shot is of, which a shader is the wrong place to
// make; a chain that wants focus pulled to a target writes the distance into a
// node parameter and names its own shader. The default is set past the near
// field so the effect shows on a scene nobody configured.
//
// **The blur is a disc, not a box.** Out-of-focus points take the shape of the
// aperture, and a square bokeh is the one artefact people identify immediately
// as wrong.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D colourImage;
layout(set = 2, binding = 1) uniform sampler2D depthImage;

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

const float FOCUS_DISTANCE = 12.0;
const float FOCUS_RANGE = 8.0;
const float MAX_RADIUS = 6.0;
const int TAPS = 12;

void main() {
	float distance = texture(depthImage, inUv).r;

	// How far out of focus, from 0 at the focus plane to 1 past the range.
	// Symmetric: something too close is as blurred as something too far.
	float confusion = clamp(abs(distance - FOCUS_DISTANCE) / FOCUS_RANGE, 0.0, 1.0);

	// **Sharp stays sharp at no cost.** Most of a frame is in focus, and taking
	// twelve taps to average a pixel with itself is the whole cost of the pass
	// paid for nothing.
	if (confusion < 0.01) {
		outColour = texture(colourImage, inUv);
		return;
	}

	float radius = confusion * MAX_RADIUS;
	vec2 texel = pass.Target.zw;

	vec4 total = texture(colourImage, inUv);
	float weight = 1.0;

	// A golden-angle spiral: the taps land evenly over the disc at any count,
	// which a ring pattern does not — rings put every sample on a few radii and
	// show as concentric banding in a large bokeh.
	const float GOLDEN = 2.39996323;
	for (int index = 0; index < TAPS; index++) {
		float spread = (float(index) + 0.5) / float(TAPS);
		float angle = float(index) * GOLDEN;
		vec2 offset = vec2(cos(angle), sin(angle)) * sqrt(spread) * radius * texel;

		// **Weighted by the neighbour's own blur, not this pixel's.** A sharp
		// foreground object must not bleed into the blurred background behind
		// it; letting a sharp tap contribute is exactly that bleed.
		float around = texture(depthImage, inUv + offset).r;
		float theirs = clamp(abs(around - FOCUS_DISTANCE) / FOCUS_RANGE, 0.0, 1.0);
		float take = step(0.01, theirs);

		total += texture(colourImage, inUv + offset) * take;
		weight += take;
	}

	outColour = total / weight;
}
