#version 450

// Contrast-adaptive sharpening.
//
// **Adaptive rather than a fixed unsharp mask, and the difference is the point.**
// A constant amount sharpens flat areas as hard as edges, which turns sensor or
// temporal noise into crawling grain — and a temporal resolve is exactly what
// this usually follows. Scaling the amount by local contrast means a smooth sky
// is left alone and an edge is not.
//
// Shipped by the engine as `sharpen`'s default.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D sourceImage;

float Luminance(vec3 colour) {
	return dot(colour, vec3(0.299, 0.587, 0.114));
}

void main() {
	vec2 texel = 1.0 / vec2(textureSize(sourceImage, 0));

	vec3 centre = texture(sourceImage, inUv).rgb;
	vec3 up = texture(sourceImage, inUv + vec2(0.0, -texel.y)).rgb;
	vec3 down = texture(sourceImage, inUv + vec2(0.0, texel.y)).rgb;
	vec3 left = texture(sourceImage, inUv + vec2(-texel.x, 0.0)).rgb;
	vec3 right = texture(sourceImage, inUv + vec2(texel.x, 0.0)).rgb;

	// **Local contrast decides the amount.** The span between the darkest and
	// brightest neighbour is near zero on a flat surface and large on an edge.
	float lowest = min(Luminance(centre), min(min(Luminance(up), Luminance(down)),
											  min(Luminance(left), Luminance(right))));
	float highest = max(Luminance(centre), max(max(Luminance(up), Luminance(down)),
											   max(Luminance(left), Luminance(right))));
	float contrast = clamp(highest - lowest, 0.0, 1.0);

	vec3 sharpened = centre * 5.0 - (up + down + left + right);
	outColour = vec4(clamp(mix(centre, sharpened * 0.25 + centre * 0.75, contrast), 0.0, 1.0), 1.0);
}
