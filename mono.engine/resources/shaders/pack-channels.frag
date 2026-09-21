#version 450

// One component per input becomes one float32 output lane. Sampling is by
// pixel centre and texelFetch, so a half-resolution source such as SSAO follows
// the capture packing rule without a filtering sampler changing its value.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outPacked;

layout(set = 2, binding = 0) uniform sampler2D rImage;
layout(set = 2, binding = 1) uniform sampler2D gImage;
layout(set = 2, binding = 2) uniform sampler2D bImage;
layout(set = 2, binding = 3) uniform sampler2D aImage;

layout(set = 3, binding = 0) uniform Components {
	uvec4 selector;
} components;

float ComponentAt(sampler2D image, uint component) {
	ivec2 extent = textureSize(image, 0);
	ivec2 pixel = clamp(ivec2(inUv * vec2(extent)), ivec2(0), extent - 1);
	return texelFetch(image, pixel, 0)[component];
}

void main() {
	outPacked = vec4(
		ComponentAt(rImage, components.selector.r),
		ComponentAt(gImage, components.selector.g),
		ComponentAt(bImage, components.selector.b),
		ComponentAt(aImage, components.selector.a)
	);
}
