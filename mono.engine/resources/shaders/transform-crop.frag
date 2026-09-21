#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D sourceImage;
layout(set = 3, binding = 0, std140) uniform Pass {
	mat4 ViewProjection;
	mat4 InverseViewProjection;
	vec4 Target;
	vec4 View;
	vec4 Eye;
	vec4 CameraDepth;
	uvec4 RenderFeatures;
	vec4 Parameters[3];
} pass;

vec2 MirrorRepeat(vec2 value) {
	vec2 repeated = fract(value * 0.5) * 2.0;
	return 1.0 - abs(repeated - 1.0);
}

void main() {
	vec4 transform = pass.Parameters[0];
	vec4 cropAndRotation = pass.Parameters[1];
	vec2 centred = inUv - vec2(0.5) - transform.zw;
	float sine = sin(-cropAndRotation.x);
	float cosine = cos(-cropAndRotation.x);
	vec2 sourceUv = mat2(cosine, -sine, sine, cosine) * centred / transform.xy + vec2(0.5);
	vec2 low = min(cropAndRotation.yz, vec2(cropAndRotation.w, pass.Parameters[2].x));
	vec2 high = max(cropAndRotation.yz, vec2(cropAndRotation.w, pass.Parameters[2].x));
	vec2 span = max(high - low, vec2(1e-6));
	vec2 normalized = (sourceUv - low) / span;
	int extendMode = int(round(pass.Parameters[2].y));
	if (extendMode == 0 && any(bvec4(lessThan(sourceUv, low), greaterThan(sourceUv, high)))) {
		outColour = vec4(0.0);
		return;
	}
	if (extendMode == 1) sourceUv = clamp(sourceUv, low, high);
	if (extendMode == 2) sourceUv = low + fract(normalized) * span;
	if (extendMode == 3) sourceUv = low + MirrorRepeat(normalized) * span;
	outColour = texture(sourceImage, sourceUv);
}
