#version 450
#extension GL_GOOGLE_include_directive : require

#include "projection.glsl"

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outOcclusion;

layout(set = 2, binding = 0) uniform sampler2D depthImage;
layout(set = 2, binding = 1) uniform sampler2D normalImage;

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
	vec4 CameraDepth;
	vec4 FogColour;
	vec4 Fog;
	vec4 Shadow;
} pass;

const int SAMPLE_COUNT = 12;
const float WORLD_RADIUS = 0.65;

float Hash(vec2 value) {
	return fract(sin(dot(value, vec2(12.9898, 78.233))) * 43758.5453);
}

vec3 WorldAt(vec2 uv, float distance) {
	return WorldAtLinearDepth(pass.InverseViewProjection, pass.CameraDepth, uv, distance);
}

void main() {
	float centre = texture(depthImage, inUv).r;
	vec4 packed = texture(normalImage, inUv * pass.Target.zw);
	vec3 normal = packed.xyz * 2.0 - 1.0;
	if (centre >= pass.Planes.y || packed.a < 0.5) {
		outOcclusion = vec4(1.0);
		return;
	}

	normal = normalize(normal);
	vec3 origin = WorldAt(inUv, centre);
	float turn = Hash(gl_FragCoord.xy) * 6.2831853;
	float blocked = 0.0;
	for (int index = 0; index < SAMPLE_COUNT; index++) {
		float along = (float(index) + 0.5) / float(SAMPLE_COUNT);
		float angle = turn + along * 12.5663706;
		vec2 at = inUv + vec2(cos(angle), sin(angle)) * along * WORLD_RADIUS / max(centre, 1.0);
		float around = texture(depthImage, at).r;
		if (around >= pass.Planes.y) {
			continue;
		}
		vec3 delta = WorldAt(at, around) - origin;
		float span = length(delta);
		if (span > 1e-4) {
			float facing = max(dot(normal, delta / span) - 0.02, 0.0);
			blocked += facing * smoothstep(1.0, 0.0, span / WORLD_RADIUS);
		}
	}
	outOcclusion = vec4(clamp(1.0 - blocked / float(SAMPLE_COUNT), 0.0, 1.0));
}
