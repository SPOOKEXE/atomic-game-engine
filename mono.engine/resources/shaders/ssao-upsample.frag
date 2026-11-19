#version 450

// Brings half-resolution ambient occlusion up to the view's full resolution.
//
// A plain bilinear fetch would average occlusion across a depth edge, which
// draws a dark rim on the far side of every silhouette. Each of the four
// nearest half-resolution samples is weighted by its bilinear share and by how
// close its depth is to this pixel's, so a sample from the other side of an
// edge contributes almost nothing.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outOcclusion;

layout(set = 2, binding = 0) uniform sampler2D halfOcclusion;
layout(set = 2, binding = 1) uniform sampler2D depthImage;

layout(set = 3, binding = 0) uniform Pass {
	mat4 InverseViewProjection;
	mat4 LightViewProjection;
	vec4 Planes;
	vec4 Target;
	vec4 Direction;
	vec4 Ambient;
	vec4 OutdoorAmbient;
	vec4 Direct;
	vec4 Eye;
	vec4 CameraDepth;
	vec4 FogColour;
	vec4 Fog;
	vec4 Shadow;
} pass;

void main() {
	float centre = texture(depthImage, inUv).r;
	// Sky reads as unoccluded, exactly as the full-resolution pass writes it.
	if (centre >= pass.Planes.y) {
		outOcclusion = vec4(1.0);
		return;
	}

	vec2 halfSize = vec2(textureSize(halfOcclusion, 0));
	vec2 position = inUv * halfSize - 0.5;
	vec2 base = floor(position);
	vec2 blend = position - base;
	float total = 0.0;
	float weights = 0.0;
	for (int y = 0; y < 2; y++) {
		for (int x = 0; x < 2; x++) {
			vec2 texel = clamp(base + vec2(x, y), vec2(0.0), halfSize - 1.0);
			vec2 uv = (texel + 0.5) / halfSize;
			float bilinear = (x == 0 ? 1.0 - blend.x : blend.x) * (y == 0 ? 1.0 - blend.y : blend.y);
			// Relative depth difference, so the tolerance scales with distance.
			float sampleDepth = texture(depthImage, uv).r;
			float similarity = 1.0 / (1e-3 + abs(sampleDepth - centre) / max(centre, 1e-3));
			float weight = bilinear * similarity;
			total += texture(halfOcclusion, uv).r * weight;
			weights += weight;
		}
	}
	outOcclusion = vec4(weights > 0.0 ? total / weights : 1.0);
}
