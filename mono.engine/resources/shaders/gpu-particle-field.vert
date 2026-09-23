#version 450

layout(location = 0) in vec4 inPositionKind;
layout(location = 1) in vec4 inVelocityAge;

layout(set = 1, binding = 0) uniform Frame {
	mat4 ViewProjection;
	vec4 CameraRight;
	vec4 CameraUp;
	vec4 CameraForward;
	vec4 Options;
	vec4 Condensation;
	vec4 Rain;
	vec4 Debris;
	vec4 Sizes;
} frame;

layout(location = 0) out vec2 outTexCoord;
layout(location = 1) out vec4 outColour;
layout(location = 2) out vec3 outWorldPosition;

void main() {
	vec2 corner = vec2((gl_VertexIndex & 1) == 0 ? -0.5 : 0.5, (gl_VertexIndex & 2) == 0 ? -0.5 : 0.5);
	float kind = inPositionKind.w;
	float size = kind < 0.5 ? frame.Sizes.x : kind < 1.5 ? frame.Sizes.y : kind < 2.5 ? frame.Sizes.z : 0.0;
	vec4 visual = kind < 0.5 ? frame.Condensation : kind < 1.5 ? frame.Rain : frame.Debris;
	vec3 right = normalize(cross(frame.CameraUp.xyz, frame.CameraForward.xyz));
	vec3 world = inPositionKind.xyz + right * corner.x * size + frame.CameraUp.xyz * corner.y * size;
	outTexCoord = corner + 0.5;
	outColour = visual;
	outWorldPosition = world;
	gl_Position = frame.ViewProjection * vec4(world, 1.0);
}
