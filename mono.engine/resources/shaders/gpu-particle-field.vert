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
		// A million parcels overlap along one viewing ray. Keep each parcel thin
		// enough that the blend integrates to cloud density instead of an opaque
		// tube. The column tapers before the cloud deck, so upper parcels may not
		// become a detached rectangular cap.
		size *= mix(0.92, 1.80, pow(smoothstep(0.03, 0.76, height), 0.72)) * mix(0.76, 1.26, seed);
		float luminance = dot(visual.rgb, vec3(0.2126, 0.7152, 0.0722));
		visual.rgb = mix(visual.rgb, vec3(luminance), 0.78) * mix(0.78, 1.08, seed);
		visual.a *= mix(0.00040, 0.00105, smoothstep(0.08, 0.64, height));
	}
	vec3 right = normalize(cross(frame.CameraUp.xyz, frame.CameraForward.xyz));
	vec3 world = inPositionKind.xyz + right * corner.x * size + frame.CameraUp.xyz * corner.y * size;
	outTexCoord = corner + 0.5;
	outColour = visual;
	outWorldPosition = world;
	outFieldData = vec4(height, cos(billowAngle), sin(billowAngle), kind);
	gl_Position = frame.ViewProjection * vec4(world, 1.0);
}
