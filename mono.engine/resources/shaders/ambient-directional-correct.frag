#version 450
#extension GL_GOOGLE_include_directive : require
#include "projection.glsl"
#include "directional-shadow.glsl"
layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;
layout(set = 2, binding = 0) uniform sampler2D baselineImage;
layout(set = 2, binding = 1) uniform sampler2D responseImage;
layout(set = 2, binding = 2) uniform sampler2D occlusionImage;
layout(set = 2, binding = 3) uniform sampler2D directionalImage;
layout(set = 2, binding = 4) uniform sampler2D depthImage;
layout(set = 2, binding = 5) uniform sampler2D normalImage;
layout(set = 2, binding = 6) uniform sampler2D shadowImage;
layout(set = 3, binding = 0) uniform Correction {
    mat4 InverseViewProjection;
    mat4 LightViewProjection;
    vec4 CameraDepth;
    vec4 Direction;
    vec4 Shadow;
} pass;
void main() {
    vec4 baseline = texture(baselineImage, inUv);
    vec4 ambient = texture(responseImage, inUv);
    vec3 colour = baseline.rgb + ambient.rgb * (texture(occlusionImage, inUv).r - ambient.a);
    vec4 packed = texture(normalImage, inUv);
    float depth = texture(depthImage, inUv).r;
    if (depth > 0.0 && packed.a >= 0.5) {
        vec3 world = WorldAtLinearDepth(pass.InverseViewProjection, pass.CameraDepth, inUv, depth);
        vec3 normal = normalize(packed.xyz * 2.0 - 1.0);
        float visibility = DirectionalShadowVisibility(shadowImage, pass.LightViewProjection,
            pass.Shadow, world, normal, -normalize(pass.Direction.xyz));
        vec4 directional = texture(directionalImage, inUv);
        colour += directional.rgb * (visibility - directional.a);
    }
    outColour = vec4(colour, baseline.a);
}
