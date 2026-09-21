// Shared per-pixel bounds for ordered transparent captures. Phase zero rejects
// opaque and already peeled fragments. Phase one replays every fragment at the
// selected depth so authored blending is preserved without a host readback.
layout(set = 2, binding = 1) uniform sampler2D layerOpaqueZ;
layout(set = 2, binding = 2) uniform sampler2D layerPreviousZ;
layout(set = 3, binding = 1, std140) uniform TransparentLayerCapture {
	vec4 Eye;
	vec4 Forward;
	vec4 Flags;
} layerCapture;

bool transparentLayerAllows(vec3 worldPosition) {
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	float distance = dot(worldPosition - layerCapture.Eye.xyz, layerCapture.Forward.xyz);
	float opaque = texelFetch(layerOpaqueZ, pixel, 0).r;
	if (distance <= 0.0 || gl_FragCoord.z >= opaque) return false;
	if (layerCapture.Flags.y != 0.0) {
		float selected = texelFetch(layerPreviousZ, pixel, 0).r;
		return selected < 1.0 && gl_FragCoord.z == selected;
	}
	if (layerCapture.Flags.x != 0.0) {
		float previous = texelFetch(layerPreviousZ, pixel, 0).r;
		return previous < 1.0 && gl_FragCoord.z > previous;
	}
	return true;
}
