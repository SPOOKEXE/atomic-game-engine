// Fullscreen UV is top-origin; SDL clip coordinates are Y-up with 0..1 depth.
vec2 ClipCoordinates(vec2 uv) {
	return vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
}

vec3 WorldAtHardwareDepth(mat4 inverseViewProjection, vec2 uv, float raw) {
	vec4 world = inverseViewProjection * vec4(ClipCoordinates(uv), raw, 1.0);
	return world.xyz / world.w;
}

// Solve the homogeneous depth equation instead of assuming a symmetric
// perspective lens. Orthographic and oblique near planes use the same path.
vec3 WorldAtLinearDepth(mat4 inverseViewProjection, vec4 cameraDepth, vec2 uv, float distance) {
	vec4 start = inverseViewProjection * vec4(ClipCoordinates(uv), 0.0, 1.0);
	vec4 step = inverseViewProjection[2];
	float raw = (distance * start.w - dot(cameraDepth, start)) /
		(dot(cameraDepth, step) - distance * step.w);
	vec4 world = start + raw * step;
	return world.xyz / world.w;
}
