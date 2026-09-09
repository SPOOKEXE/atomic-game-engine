#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;
layout(set = 2, binding = 0) uniform sampler2D foregroundColour;
layout(set = 2, binding = 1) uniform sampler2D backgroundColour;

void main() {
	vec4 foreground = texture(foregroundColour, inUv);
	vec4 background = texture(backgroundColour, inUv);
	outColour = foreground + background * (1.0 - clamp(foreground.a, 0.0, 1.0));
}
