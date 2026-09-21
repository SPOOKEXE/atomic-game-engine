#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;
layout(location = 1) out float outDepth;

layout(set = 2, binding = 0) uniform sampler2D foregroundColour;
layout(set = 2, binding = 1) uniform sampler2D foregroundDepth;
layout(set = 2, binding = 2) uniform sampler2D backgroundColour;
layout(set = 2, binding = 3) uniform sampler2D backgroundDepth;

layout(set = 3, binding = 0, std140) uniform Composition {
	uint foregroundMode;
} composition;

void main() {
	float front = texture(foregroundDepth, inUv).r;
	float back = texture(backgroundDepth, inUv).r;
	bool visible = front > 0.0 && (back == 0.0 || front < back);
	vec4 background = texture(backgroundColour, inUv);
	vec4 foreground = texture(foregroundColour, inUv);
	if (composition.foregroundMode != 0u) {
		// Ordered glass must not become an opaque occluder for the next layer.
		float alpha = visible ? clamp(foreground.a, 0.0, 1.0) : 0.0;
		float weight = composition.foregroundMode == 1u ? alpha : 1.0;
		outColour = visible
			? vec4(foreground.rgb * weight + background.rgb * (1.0 - alpha),
			       alpha + background.a * (1.0 - alpha))
			: background;
		outDepth = back;
	} else {
		outColour = visible ? foreground : background;
		outDepth = visible ? front : back;
	}
}
