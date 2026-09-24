#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 rendered;
layout(location = 1) out vec4 depth;

layout(set = 2, binding = 0) uniform sampler2D frontSurface;
layout(set = 2, binding = 1) uniform sampler2D backSurface;
layout(set = 3, binding = 0) uniform TransformOutput {
	vec2 tiling;
	vec2 depthRange;
	uint useBackSurface;
} transformOutput;

void main() {
	if (transformOutput.useBackSurface != 0) {
		rendered = texture(backSurface, uv * transformOutput.tiling);
	} else {
		rendered = texture(frontSurface, uv * transformOutput.tiling);
	}
	float zNdc = gl_FragCoord.z;
	float encoded = 1.0 - ((zNdc - transformOutput.depthRange.x) /
		(transformOutput.depthRange.y - transformOutput.depthRange.x));
	depth = vec4(encoded, encoded, encoded, 1.0);
}
