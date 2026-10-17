#version 450
#extension GL_GOOGLE_include_directive : require

// Grid-only transparent composition: preserve the incoming image without a
// preceding copy, and sample opaque depth without writing or cycling it.
layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;
layout(set = 2, binding = 0) uniform sampler2D sourceImage;
layout(set = 2, binding = 1) uniform sampler2D opaqueDepth;

#include "grid.glsl"

void main() {
	// Integer pixel reads ignore sampler filtering and rounded backing extents.
	const ivec2 pixel = ivec2(gl_FragCoord.xy);
	const vec4 source = texelFetch(sourceImage, pixel, 0);
	outColour = source;
	vec4 colour;
	float depth;
	if (!EvaluateGroundGrid(inUv, colour, depth)) return;
	// GridPipeline inherits the opaque pipeline's strict LESS comparison.
	if (!(depth < texelFetch(opaqueDepth, pixel, 0).r)) return;
	const float remaining = 1.0 - colour.a;
	outColour = vec4(colour.rgb * colour.a + source.rgb * remaining,
		colour.a + source.a * remaining);
}
