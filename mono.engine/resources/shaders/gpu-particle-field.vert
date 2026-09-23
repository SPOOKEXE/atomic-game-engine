#version 450

layout(location = 0) in vec4 inPositionKind;
layout(location = 1) in vec4 inVelocityAge;

layout(set = 1, binding = 0) uniform Frame {
	mat4 ViewProjection;
	vec4 CameraRight;
	vec4 CameraUp;
	vec4 CameraForward;
	// top height, storm centre Y, seed, requested depth slice
	vec4 Options;
	// storm centre XYZ, influence radius
	vec4 FieldBounds;
	// core radius, unused
	vec4 FieldDensity;
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
	// Twelve far-to-near passes provide deterministic alpha ordering for the
	// small visible condensation cohort. The full field remains compute-only.
	float depthExtent = frame.FieldBounds.w * 1.80 + frame.Options.x * 0.35;
	float depth = dot(inPositionKind.xyz - frame.FieldBounds.xyz, frame.CameraForward.xyz);
	int depthSlice = int(floor(clamp(depth / max(depthExtent, 1.0) * 0.5 + 0.5, 0.0, 0.999999) * 12.0));
	if (kind < 0.5 && depthSlice != int(frame.Options.w)) {
		gl_Position = vec4(-4.0, -4.0, 0.0, 1.0);
		outTexCoord = vec2(0.0);
		outColour = vec4(0.0);
		outWorldPosition = inPositionKind.xyz;
		outFieldData = vec4(height, cos(billowAngle), sin(billowAngle), kind);
		return;
	}
	if (kind < 0.5) {
		// Match the reference's coarse/fine cloud-density gate. It leaves a
		// small parcel floor for turbulent edges while concentrating optical depth
		// on the analytical funnel wall.
		float coreRadius = max(frame.FieldDensity.x, 1.0);
		float radialDistance = length(inPositionKind.xz - frame.FieldBounds.xz);
		float radiusScale = min(1.0, 500.0 / max(frame.Options.x, 1.0));
		float funnelRadius = coreRadius * (0.38 + 0.0036 * (height * frame.Options.x) * radiusScale);
		float funnelDensity = exp(-pow((radialDistance - funnelRadius) / max(coreRadius * 0.34, 2.0), 2.0));
		float wallDensity = exp(-pow((radialDistance / coreRadius - 1.8) / 1.7, 2.0))
			* exp(-pow((height - 0.24) / 0.22, 2.0)) * 0.82;
		float renderDensity = max(max(funnelDensity, wallDensity), 0.10);
		// A million parcels overlap along one viewing ray. Keep each parcel thin
		// enough that the blend integrates to cloud density instead of an opaque
		// tube. The column tapers before the cloud deck, so upper parcels may not
		// become a detached rectangular cap.
		size *= mix(0.92, 1.42, pow(smoothstep(0.03, 0.68, height), 0.72)) * mix(0.82, 1.54, renderDensity) * mix(0.76, 1.26, seed);
		float luminance = dot(visual.rgb, vec3(0.2126, 0.7152, 0.0722));
		visual.rgb = mix(visual.rgb, vec3(luminance), 0.78) * mix(0.78, 1.08, seed);
		// The depth-sliced path draws a bounded 18k cohort, unlike the former
		// all-row path. Its per-parcel optical depth can therefore match the
		// reference cloud budget instead of being diluted for a million billboards.
		visual.a *= mix(0.06, 0.16, smoothstep(0.08, 0.64, height)) * renderDensity;
	}
	vec3 right = normalize(cross(frame.CameraUp.xyz, frame.CameraForward.xyz));
	vec3 world = inPositionKind.xyz + right * corner.x * size + frame.CameraUp.xyz * corner.y * size;
	outTexCoord = corner + 0.5;
	outColour = visual;
	outWorldPosition = world;
	outFieldData = vec4(height, cos(billowAngle), sin(billowAngle), kind);
	gl_Position = frame.ViewProjection * vec4(world, 1.0);
}
