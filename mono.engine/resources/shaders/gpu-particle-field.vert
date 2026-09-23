#version 450

layout(location = 0) in vec4 inPositionKind;
layout(location = 1) in vec4 inVelocityAge;

layout(set = 1, binding = 0) uniform Frame {
	mat4 ViewProjection;
	vec4 CameraRight;
	vec4 CameraUp;
	vec4 CameraForward;
	// top height, storm centre Y, seed, unused
	vec4 Options;
	vec4 Condensation;
	vec4 Rain;
	vec4 Debris;
	vec4 Sizes;
} frame;

layout(location = 0) out vec2 outTexCoord;
layout(location = 1) out vec4 outColour;
layout(location = 2) out vec3 outWorldPosition;
layout(location = 3) out vec4 outFieldData;

void main() {
	vec2 corner = vec2((gl_VertexIndex & 1) == 0 ? -0.5 : 0.5, (gl_VertexIndex & 2) == 0 ? -0.5 : 0.5);
	float kind = inPositionKind.w;
	float size = kind < 0.5 ? frame.Sizes.x : kind < 1.5 ? frame.Sizes.y : kind < 2.5 ? frame.Sizes.z : 0.0;
	vec4 visual = kind < 0.5 ? frame.Condensation : kind < 1.5 ? frame.Rain : frame.Debris;
	float height = clamp((inPositionKind.y - frame.Options.y) / max(frame.Options.x, 1.0), 0.0, 1.0);
	float seed = fract(sin(dot(inPositionKind.xz, vec2(0.071, 0.113)) + frame.Options.z) * 43758.5453);
	float billowAngle = seed * 6.28318530718;
	if (kind < 0.5) {
		// Elevated parcels overlap more billows, hiding the regularity a fixed
		// billboard size exposes while preserving the authored base size.
		size *= mix(1.20, 2.35, pow(smoothstep(0.03, 0.94, height), 0.72)) * mix(0.84, 1.14, seed);
		visual.rgb *= mix(0.78, 1.12, seed);
		visual.a *= mix(1.35, 1.80, smoothstep(0.06, 0.72, height));
	}
	vec3 right = normalize(cross(frame.CameraUp.xyz, frame.CameraForward.xyz));
	vec3 world = inPositionKind.xyz + right * corner.x * size + frame.CameraUp.xyz * corner.y * size;
	outTexCoord = corner + 0.5;
	outColour = visual;
	outWorldPosition = world;
	outFieldData = vec4(height, cos(billowAngle), sin(billowAngle), kind);
	gl_Position = frame.ViewProjection * vec4(world, 1.0);
}
