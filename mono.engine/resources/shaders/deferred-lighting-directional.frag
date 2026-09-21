#version 450
#extension GL_GOOGLE_include_directive : require
#include "deferred-lighting.glsl"
layout(location = 0) out vec4 outColour;
layout(location = 1) out vec4 outBaseline;
layout(location = 2) out vec4 outDirectionalResponse;
void main() {
    vec4 lighting = ShadeDeferred(outDirectionalResponse);
    outColour = lighting;
    outBaseline = lighting;
}
