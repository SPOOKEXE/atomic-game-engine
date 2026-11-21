#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D colourImage;
layout(set = 2, binding = 1) uniform sampler2D depthImage;
layout(set = 3, binding = 0, std140) uniform LightingEffects {
	// x: intensity, y: focus distance, z: focus range, w: radius in pixels.
	vec4 depthOfField;
	vec4 godRays;
	// zw: one target pixel in UV space.
	vec4 target;
} effects;

// Disc samples per blurred pixel, placed on a golden-angle spiral so the
// disc is covered evenly at any radius without a repeating pattern.
const int SAMPLES = 24;
const float GOLDEN_ANGLE = 2.39996323;

// Blur radius in target pixels for a linear view depth: zero inside the focus
// range, growing to the full radius one range further out.
float BlurPixels(float depth) {
	float focusRange = max(effects.depthOfField.z, 0.0001);
	float circle = clamp((abs(depth - effects.depthOfField.y) - focusRange) / focusRange, 0.0, 1.0);
	return effects.depthOfField.w * effects.depthOfField.x * circle;
}

// Depth at the nearest texel, so the blur size of an edge sample belongs to
// one surface rather than an interpolation of two.
float DepthAt(vec2 uv) {
	ivec2 size = textureSize(depthImage, 0);
	return texelFetch(depthImage, clamp(ivec2(uv * vec2(size)), ivec2(0), size - 1), 0).r;
}

void main() {
	ivec2 outputSize = ivec2(round(1.0 / effects.target.zw));
	bool samePixelGrid = all(equal(textureSize(colourImage, 0), outputSize)) &&
		all(equal(textureSize(depthImage, 0), outputSize));
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	vec4 source = samePixelGrid ? texelFetch(colourImage, pixel, 0) : texture(colourImage, inUv);
	if (effects.depthOfField.x <= 0.0 || effects.depthOfField.w <= 0.0) {
		outColour = source;
		return;
	}

	float depth = samePixelGrid ? texelFetch(depthImage, pixel, 0).r : DepthAt(inUv);
	float size = BlurPixels(depth);
	if (size <= 0.0) {
		// Focused pixels preserve their source instead of resampling across colour edges.
		outColour = source;
		return;
	}

	// Gather over this pixel's circle of confusion. A sample counts only when
	// its own blur reaches this pixel, so a sharp object does not smear into a
	// blurred background behind it; a sample further away than this pixel may
	// spread at most twice this pixel's blur, so the background does not wash
	// over a blurred foreground. Each sample is weighted by how far it
	// reaches and the sum is normalised by those weights, so the blur only
	// redistributes light. A refused sample used to add the running mean,
	// which repeated whatever the early, inner samples had seen: a small
	// bright object near the centre was counted many times and glowed.
	vec4 accumulated = source;
	float weight = 1.0;
	for (int index = 0; index < SAMPLES; ++index) {
		float radius = size * sqrt((float(index) + 0.5) / float(SAMPLES));
		float angle = float(index) * GOLDEN_ANGLE;
		vec2 uv = inUv + vec2(cos(angle), sin(angle)) * radius * effects.target.zw;
		float sampleDepth = DepthAt(uv);
		float sampleSize = BlurPixels(sampleDepth);
		if (sampleDepth > depth) sampleSize = min(sampleSize, size * 2.0);
		float reach = smoothstep(radius - 0.5, radius + 0.5, sampleSize);
		accumulated += texture(colourImage, uv) * reach;
		weight += reach;
	}
	outColour = accumulated / weight;
}
