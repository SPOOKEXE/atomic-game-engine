#version 450
#extension GL_GOOGLE_include_directive : require
#include "deferred-lighting.glsl"
layout(location = 0) out vec4 outColour;
layout(location = 1) out vec4 outBaseline;
void main() {
    vec4 lighting = ShadeDeferred();
    outColour = lighting;
    outBaseline = lighting;
}
