#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;
layout(set = 2, binding = 0) uniform sampler2D encodedImage;
layout(set = 3, binding = 0, std140) uniform Publication {
	uvec4 Control;
}
publication;

vec3 Decode(vec3 encoded) {
	return mix(
		encoded / 12.92, pow((encoded + 0.055) / 1.055, vec3(2.4)), greaterThan(encoded, vec3(0.04045))
	);
}
void main() {
	ivec2 size = textureSize(encodedImage, 0);
	ivec2 pixel = clamp(ivec2(inUv * vec2(size)), ivec2(0), size - 1);
	vec4 encoded = texelFetch(encodedImage, pixel, 0);
	// The sRGB colour target encodes these linear values back into the authored
	// bytes. Ordinary image/material consumers then receive correct sampled colour.
	outColour = publication.Control.x == 1u ? vec4(Decode(encoded.rgb), encoded.a) : encoded;
}
