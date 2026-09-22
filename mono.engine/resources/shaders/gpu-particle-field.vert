#version 450

layout(location = 0) in vec4 inPositionKind;
layout(location = 1) in vec4 inVelocityAge;

layout(set = 1, binding = 0) uniform Frame {
	mat4 ViewProjection;
	vec4 CameraRight;
	vec4 CameraUp;
	vec4 CameraForward;
	vec4 Options;
} frame;

layout(location = 0) out vec2 outTexCoord;
layout(location = 1) out vec4 outColour;
layout(location = 2) out vec3 outWorldPosition;

void main() {
	vec2 corner = vec2((gl_VertexIndex & 1) == 0 ? -0.5 : 0.5, (gl_VertexIndex & 2) == 0 ? -0.5 : 0.5);
	float kind = inPositionKind.w;
	float size = kind < 0.5 ? 3.0 : kind < 1.5 ? 0.45 : kind < 2.5 ? 0.8 : 0.0;
	vec3 colour = kind < 0.5 ? vec3(0.70, 0.74, 0.76) : kind < 1.5 ? vec3(0.55, 0.67, 0.85) : vec3(0.34, 0.25, 0.16);
	float alpha = kind < 0.5 ? 0.11 : kind < 1.5 ? 0.35 : kind < 2.5 ? 0.24 : 0.0;
	vec3 right = normalize(cross(frame.CameraUp.xyz, frame.CameraForward.xyz));
	vec3 world = inPositionKind.xyz + right * corner.x * size + frame.CameraUp.xyz * corner.y * size;
	outTexCoord = corner + 0.5;
	outColour = vec4(colour, alpha);
	outWorldPosition = world;
	gl_Position = frame.ViewProjection * vec4(world, 1.0);
}
