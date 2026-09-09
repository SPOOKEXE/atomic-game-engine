#version 450
layout(location = 0) in vec2 inUv;
layout(location = 0) out float outDepth;
layout(location = 1) out vec4 outNormal;
layout(set = 2, binding = 0) uniform sampler2D bodyDepth;
layout(set = 2, binding = 1) uniform sampler2D bodyNormal;
layout(set = 2, binding = 2) uniform sampler2D roomDepth;
layout(set = 2, binding = 3) uniform sampler2D roomNormal;
layout(set = 3, binding = 0, std140) uniform Merge { vec4 parameters; } merge;
void main() {
    float body = texture(bodyDepth, inUv).r;
    float room = texture(roomDepth, inUv).r;
    bool bodyVisible = body > 0.0 && body < merge.parameters.x && (room == 0.0 || body < room);
    outDepth = bodyVisible ? body : (room > 0.0 ? room : merge.parameters.x);
    outNormal = bodyVisible ? texture(bodyNormal, inUv * merge.parameters.yz) : texture(roomNormal, inUv);
}
