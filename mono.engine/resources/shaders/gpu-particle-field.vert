#version 450
layout(location = 0) in vec4 inPositionLayer;
layout(location = 1) in vec4 inVelocityAge;
layout(set = 1, binding = 0) uniform Frame {
	mat4 ViewProjection;
	vec4 CameraRight, CameraUp, CameraForward, Centre, Options;
	vec4 Colours[3];
	vec4 Sizes;
} frame;
layout(location = 0) out vec2 outTexCoord;
layout(location = 1) out vec4 outColour;
layout(location = 2) out vec3 outWorldPosition;
void main() {
	uint layer = uint(max(inPositionLayer.w, 0.0));
	float depth = dot(inPositionLayer.xyz - frame.Centre.xyz, frame.CameraForward.xyz);
	int slice = int(floor(clamp(depth / max(frame.Centre.w, 1.0) * 0.5 + 0.5, 0.0, 0.999999) * 12.0));
	if (layer >= 3u || (uint(frame.Options.y) & (1u << layer)) == 0u || slice != int(frame.Options.x)) {
		gl_Position = vec4(-4.0, -4.0, 0.0, 1.0); outColour = vec4(0.0); outTexCoord = vec2(0.0); outWorldPosition = inPositionLayer.xyz; return;
	}
	vec2 corner = vec2((gl_VertexIndex & 1) == 0 ? -0.5 : 0.5, (gl_VertexIndex & 2) == 0 ? -0.5 : 0.5);
	vec3 world = inPositionLayer.xyz + frame.CameraRight.xyz * corner.x * frame.Sizes[layer] + frame.CameraUp.xyz * corner.y * frame.Sizes[layer];
	outTexCoord = corner + 0.5; outColour = frame.Colours[layer]; outWorldPosition = world;
	gl_Position = frame.ViewProjection * vec4(world, 1.0);
}
