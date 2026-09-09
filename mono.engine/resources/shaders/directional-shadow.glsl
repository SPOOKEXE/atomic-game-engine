// Native directional PCF is shared by shading and retained radiance correction.
float DirectionalShadowVisibility(sampler2D shadowImage, mat4 lightViewProjection, vec4 shadow, vec3 world, vec3 normal, vec3 toLight) {
	if (shadow.x < 0.5) {
		return 1.0;
	}
	vec4 lightPosition = lightViewProjection * vec4(world, 1.0);
	vec3 projected = lightPosition.xyz / max(lightPosition.w, 1e-6);
	vec2 uv = vec2(projected.x * 0.5 + 0.5, 0.5 - projected.y * 0.5);
	if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || projected.z > 1.0) {
		return 1.0;
	}
	float bias = 0.0015 + 0.0045 * (1.0 - max(dot(normal, toLight), 0.0));
	vec2 texel = vec2(shadow.y);
	vec2 offsets[4] = vec2[4](
		vec2(-0.5, -0.5) * texel, vec2(0.5, -0.5) * texel, vec2(-0.5, 0.5) * texel, vec2(0.5, 0.5) * texel
	);
	float lit = 0.0;
	for (int index = 0; index < 4; index++) {
		lit += projected.z - bias <= texture(shadowImage, uv + offsets[index]).r ? 1.0 : 0.0;
	}
	return lit * 0.25;
}

