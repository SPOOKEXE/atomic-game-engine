#version 450
#extension GL_GOOGLE_include_directive : require
#include "projection.glsl"
#include "ambient-lighting.glsl"
layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outResponse;
layout(set = 2, binding = 0) uniform sampler2D albedoImage;
layout(set = 2, binding = 1) uniform sampler2D normalImage;
layout(set = 2, binding = 2) uniform sampler2D materialImage;
layout(set = 2, binding = 3) uniform sampler2D depthImage;
layout(set = 2, binding = 4) uniform sampler2D occlusionImage;
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

void main() {
    vec2 geometryUv = inUv * pass.Target.zw;
    vec4 packed = texture(normalImage, geometryUv);
    float depth = texture(depthImage, inUv).r;
    float originalAo = texture(occlusionImage, inUv).r;
    outResponse = vec4(0.0, 0.0, 0.0, originalAo);
    if (depth >= pass.Planes.y || packed.a < 0.5) return;
    vec3 normal = normalize(packed.xyz * 2.0 - 1.0);
    vec3 albedo = texture(albedoImage, geometryUv).rgb;
    vec3 material = texture(materialImage, geometryUv).rgb;
    vec3 world = WorldAtLinearDepth(pass.InverseViewProjection, pass.CameraDepth, inUv, depth);
    float fog = clamp((distance(world, pass.Eye.xyz) - pass.Fog.x) /
                      max(pass.Fog.y - pass.Fog.x, 0.0001), 0.0, 1.0);
    outResponse.rgb = AmbientRadiance(albedo, clamp(material.g, 0.0, 1.0), normal,
                                     pass.Ambient.rgb, pass.OutdoorAmbient.rgb, material.b) * (1.0 - fog);
}
