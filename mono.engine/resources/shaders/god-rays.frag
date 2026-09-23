#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D colourImage;
layout(set = 2, binding = 1) uniform sampler2D linearDepthImage;
layout(set = 3, binding = 0, std140) uniform LightingEffects {
	// x: the linear-depth clear value, which is the view far plane.
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
		// Dedicated effect graphs may clear depth to zero, while the standard
		// linear-depth pass clears it to the view far plane. Both mean sky; an
		// in-range value is opaque and blocks the sampled radiance.
		float depth = texture(linearDepthImage, sampleUv).r;
		float unobscured = depth <= 0.0 || depth >= effects.depthOfField.x - 0.001 ? 1.0 : 0.0;
		shafts += Extract(texture(colourImage, sampleUv).rgb) * unobscured * (1.0 - fraction * 0.5);
	}
	outColour = vec4(source.rgb + shafts * effects.godRays.x / 8.0, source.a);
}
