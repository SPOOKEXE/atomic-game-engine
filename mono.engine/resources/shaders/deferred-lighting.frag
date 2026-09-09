#version 450
#extension GL_GOOGLE_include_directive : require
#include "deferred-lighting.glsl"
layout(location = 0) out vec4 outColour;
void main() {
    outColour = ShadeDeferred();
}
