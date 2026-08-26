#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D litImage;
layout(set = 2, binding = 1) uniform sampler2D depthImage;
layout(set = 2, binding = 2) uniform sampler2D environmentImage;

layout(set = 3, binding = 0) uniform Pass {
	mat4 InverseViewProjection;
	mat4 LightViewProjection;
	vec4 Planes;
	vec4 Target;
	vec4 Direction;
	vec4 Ambient;
	vec4 OutdoorAmbient;
	vec4 Direct;
	vec4 Eye;
	vec4 FogColour;
	vec4 Fog;
	vec4 Shadow;
} pass;

const float PI = 3.14159265358979323846;

void main() {
	vec4 lit = texture(litImage, inUv);
	float depth = texture(depthImage, inUv * pass.Target.zw).r;
	if (pass.Fog.w < 0.5 || depth < 0.999999) {
		outColour = lit;
		return;
	}

	vec2 clip = vec2(inUv.x * 2.0 - 1.0, 1.0 - inUv.y * 2.0);
	vec4 farPoint = pass.InverseViewProjection * vec4(clip, 1.0, 1.0);
	vec3 world = farPoint.xyz / max(abs(farPoint.w), 1e-6);
	vec3 direction = normalize(world - pass.Eye.xyz);
	vec2 environmentUv = vec2(
		atan(direction.x, direction.z) / (2.0 * PI) + 0.5,
		asin(clamp(direction.y, -1.0, 1.0)) / -PI + 0.5
	);
	outColour = vec4(texture(environmentImage, environmentUv).rgb, lit.a);
}
