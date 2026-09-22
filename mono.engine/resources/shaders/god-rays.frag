#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D colourImage;
layout(set = 2, binding = 1) uniform sampler2D linearDepthImage;
layout(set = 3, binding = 0, std140) uniform LightingEffects {
	vec4 depthOfField;
	// x: intensity, y: HDR threshold, z: radius in pixels, w: projected-sun visibility.
	vec4 godRays;
	// xy: projected sun UV, zw: one target pixel in UV space.
	vec4 target;
} effects;

vec3 Extract(vec3 colour) {
	return max(colour - vec3(effects.godRays.y), vec3(0.0));
}

void main() {
	vec4 source = texture(colourImage, inUv);
	if (effects.godRays.x <= 0.0 || effects.godRays.z <= 0.0 || effects.godRays.w <= 0.0) {
		outColour = source;
		return;
	}

	vec2 toSun = effects.target.xy - inUv;
	float rayLength = length(toSun);
	vec2 direction = rayLength > 0.0001 ? toSun / rayLength : vec2(0.0);
	float sampleLength = min(rayLength, effects.godRays.z * max(effects.target.z, effects.target.w));
	vec3 shafts = vec3(0.0);
	for (int sampleIndex = 1; sampleIndex <= 8; ++sampleIndex) {
		float fraction = float(sampleIndex) / 8.0;
		vec2 sampleUv = clamp(inUv + direction * sampleLength * fraction, vec2(0.0), vec2(1.0));
		// The linear-depth pass writes zero for sky. Opaque geometry blocks the
		// sampled radiance, so shafts do not continue through foreground solids.
		float unobscured = texture(linearDepthImage, sampleUv).r <= 0.0 ? 1.0 : 0.0;
		shafts += Extract(texture(colourImage, sampleUv).rgb) * unobscured * (1.0 - fraction * 0.5);
	}
	outColour = vec4(source.rgb + shafts * effects.godRays.x / 8.0, source.a);
}
