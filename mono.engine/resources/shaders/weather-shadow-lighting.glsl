// Separate uniform slot preserves the existing built-in lighting prefix.
#ifndef WEATHER_SHADOW_SAMPLER_BINDING
#define WEATHER_SHADOW_SAMPLER_BINDING 10
#endif
layout(set = 2, binding = WEATHER_SHADOW_SAMPLER_BINDING) uniform sampler2D weatherShadowImage;
#ifndef WEATHER_SHADOW_INLINE_STATE
layout(set = 3, binding = 3) uniform WeatherShadowLighting {
	vec4 Regions[4];
	vec4 Controls;
	vec4 ToSun;
	vec4 Receiver;
} weatherShadow;
#endif

float WeatherShadowFactor(vec3 world) {
	uint cascades = uint(weatherShadow.Controls.w);
	if (cascades == 0u || weatherShadow.Controls.x <= 0.0 || weatherShadow.ToSun.y <= 0.0 ||
		world.y >= weatherShadow.Controls.z) return 1.0;
	float receiverDistance = length(world.xz - weatherShadow.Receiver.xy);
	float distanceFade = 1.0 - smoothstep(weatherShadow.ToSun.w * 0.8, weatherShadow.ToSun.w, receiverDistance);
	if (distanceFade <= 0.0) return 1.0;
	vec2 bottom = world.xz + weatherShadow.ToSun.xz *
		((weatherShadow.Controls.y - world.y) / weatherShadow.ToSun.y);
	for (uint cascade = 0u; cascade < cascades; ++cascade) {
		vec4 region = weatherShadow.Regions[cascade];
		vec2 local = (bottom - region.xy) / (2.0 * region.z) + 0.5;
		if (any(lessThan(local, vec2(0.0))) || any(greaterThan(local, vec2(1.0)))) continue;
		vec2 filtered = clamp(local, vec2(region.w), vec2(1.0 - region.w));
		vec2 atlas = vec2(filtered.x, (float(cascade) + filtered.y) / float(cascades));
		vec4 bands = texture(weatherShadowImage, atlas);
		float height = clamp((world.y - weatherShadow.Controls.y) /
			max(weatherShadow.Controls.z - weatherShadow.Controls.y, 1e-4), 0.0, 1.0) * 4.0;
		float opticalDepth = 0.0;
		for (uint band = 0u; band < 4u; ++band)
			opticalDepth += bands[band] * clamp(float(band + 1u) - height, 0.0, 1.0);
		float edge = min(min(local.x, local.y), min(1.0 - local.x, 1.0 - local.y));
		float edgeFade = smoothstep(0.0, 0.08, edge);
		if (edgeFade < 1.0 && cascade + 1u < cascades) {
			vec4 parent = weatherShadow.Regions[cascade + 1u];
			vec2 parentUv = (bottom - parent.xy) / (2.0 * parent.z) + 0.5;
			parentUv = clamp(parentUv, vec2(parent.w), vec2(1.0 - parent.w));
			vec4 parentBands = texture(weatherShadowImage,
				vec2(parentUv.x, (float(cascade + 1u) + parentUv.y) / float(cascades)));
			float parentDepth = 0.0;
			for (uint band = 0u; band < 4u; ++band)
				parentDepth += parentBands[band] * clamp(float(band + 1u) - height, 0.0, 1.0);
			opticalDepth = mix(parentDepth, opticalDepth, edgeFade);
			edgeFade = 1.0;
		}
		return mix(1.0, exp(-opticalDepth), weatherShadow.Controls.x * weatherShadow.Receiver.w *
			distanceFade * edgeFade);
	}
	return 1.0;
}
