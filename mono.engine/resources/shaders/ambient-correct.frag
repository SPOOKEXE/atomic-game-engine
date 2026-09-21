#version 450
layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;
layout(set = 2, binding = 0) uniform sampler2D baselineImage;
layout(set = 2, binding = 1) uniform sampler2D responseImage;
layout(set = 2, binding = 2) uniform sampler2D occlusionImage;
void main() {
    vec4 baseline = texture(baselineImage, inUv);
    vec4 response = texture(responseImage, inUv);
    float visibility = texture(occlusionImage, inUv).r;
    outColour = vec4(baseline.rgb + response.rgb * (visibility - response.a), baseline.a);
}
