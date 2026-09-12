#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D colourImage;
layout(set = 3, binding = 0, std140) uniform Pass {
	mat4 ViewProjection;
	mat4 InverseViewProjection;
	vec4 Target;
	vec4 View;
	uvec4 RenderFeatures;
} pass;

float Luma(vec3 colour) {
	return dot(colour, vec3(0.299, 0.587, 0.114));
}

void main() {
	vec2 texel = pass.Target.zw;
	vec3 centre = texture(colourImage, inUv).rgb;
	float northWest = Luma(texture(colourImage, inUv + vec2(-texel.x, -texel.y)).rgb);
	float northEast = Luma(texture(colourImage, inUv + vec2(texel.x, -texel.y)).rgb);
	float southWest = Luma(texture(colourImage, inUv + vec2(-texel.x, texel.y)).rgb);
	float southEast = Luma(texture(colourImage, inUv + texel).rgb);
	float middle = Luma(centre);

	vec2 direction = vec2(
		-((northWest + northEast) - (southWest + southEast)),
		(northWest + southWest) - (northEast + southEast)
	);
	float reduce = max((northWest + northEast + southWest + southEast) * 0.03125, 0.0078125);
	float reciprocal = 1.0 / (min(abs(direction.x), abs(direction.y)) + reduce);
	direction = clamp(direction * reciprocal, vec2(-8.0), vec2(8.0)) * texel;

	vec3 nearPair = 0.5 * (
		texture(colourImage, inUv + direction * (1.0 / 3.0 - 0.5)).rgb +
		texture(colourImage, inUv + direction * (2.0 / 3.0 - 0.5)).rgb
	);
	vec3 farPair = nearPair * 0.5 + 0.25 * (
		texture(colourImage, inUv + direction * -0.5).rgb +
		texture(colourImage, inUv + direction * 0.5).rgb
	);
	float farLuma = Luma(farPair);
	float low = min(middle, min(min(northWest, northEast), min(southWest, southEast)));
	float high = max(middle, max(max(northWest, northEast), max(southWest, southEast)));
	outColour = vec4(farLuma < low || farLuma > high ? nearPair : farPair, 1.0);
}
