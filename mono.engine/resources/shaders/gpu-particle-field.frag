#version 450

// The analytical field has its own cloud mask. Authored particle emitters keep
// `particle.frag`, while this shader can overlap irregular condensation billows
// without changing their texture and blend controls.

layout(location = 0) in vec2 inTexCoord;
layout(location = 1) in vec4 inColour;
layout(location = 2) in vec3 inWorldPosition;
layout(location = 3) in vec4 inFieldData;

layout(set = 2, binding = 0) uniform sampler2D particleTexture;

layout(set = 3, binding = 0) uniform Material {
	vec4 Flags;
	vec4 Illumination;
	vec4 FogColour;
	vec4 Fog;
	vec4 Eye;
} material;

layout(location = 0) out vec4 outColour;

float BillowDistance(vec2 point, vec2 direction) {
	vec2 stretch = vec2(0.82, 1.12);
	float mainLobe = length(point * stretch) / 0.70;
	vec2 first = direction * 0.20;
	vec2 second = vec2(direction.x * -0.5885011 - direction.y * 0.8084964,
		direction.x * 0.8084964 + direction.y * -0.5885011) * 0.23;
	vec2 third = vec2(direction.x * -0.3073329 - direction.y * -0.9516021,
		direction.x * -0.9516021 + direction.y * -0.3073329) * 0.17;
	float lobeA = length((point - first) * stretch) / 0.46;
	float lobeB = length((point - second) * stretch) / 0.42;
	float lobeC = length((point - third) * stretch) / 0.39;
	return min(mainLobe, min(lobeA, min(lobeB, lobeC)));
}

void main() {
	float kind = inFieldData.w;
	vec4 result = texture(particleTexture, inTexCoord) * inColour;
	if (kind < 0.5) {
		vec2 point = inTexCoord - vec2(0.5);
		vec2 billowDirection = inFieldData.yz;
		float billowDistance = BillowDistance(point, billowDirection);
		float scallop = 0.035 * dot(point, billowDirection);
		// A broad shoulder lets adjacent depth-sorted parcels merge into a cloud
		// volume. The previous narrow edge exposed every billboard as a separate
		// circular puff at the reference camera distance.
		float softness = 1.0 - smoothstep(0.20, 1.28 + scallop, billowDistance);
		// Fade the rising edge across several billows. A hard cut at the authored
		// height turns even a sparse field into a rectangular cap.
		float topFade = 1.0 - smoothstep(0.40, 0.68, inFieldData.x);
		result.a *= softness * topFade;
	} else {
		float radius = length(inTexCoord - vec2(0.5)) * 2.0;
		result.a *= 1.0 - smoothstep(0.42, 1.0, radius);
	}
	result.rgb *= mix(vec3(1.0), material.Illumination.rgb, material.Flags.z);

	float fogInterval = max(material.Fog.y - material.Fog.x, 0.0001);
	float fog = clamp((distance(inWorldPosition, material.Eye.xyz) - material.Fog.x) / fogInterval, 0.0, 1.0);
	result.rgb = mix(result.rgb, material.FogColour.rgb, fog);
	// A dense analytical field integrates thousands of very low-opacity parcels.
	// Discarding at ordinary-emitter opacity removes the field before blending.
	if (result.a < 0.00001) discard;
	result.rgb *= result.a;
	result.a *= 1.0 - material.Flags.y;
	outColour = result;
}
