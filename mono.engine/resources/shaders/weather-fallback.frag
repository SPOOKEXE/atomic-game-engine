#version 450

// Four cosmetic volume samples preserve a visible sky and cloud layer when
// compute products are unavailable. This approximation never produces physics.
layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;
layout(set = 2, binding = 0) uniform sampler2D colourImage;
layout(set = 2, binding = 1) uniform sampler2D depthImage;
layout(set = 2, binding = 2) uniform sampler2D environmentImage;
layout(set = 2, binding = 3) uniform sampler2D frontFace;
layout(set = 2, binding = 4) uniform sampler2D backFace;
layout(set = 2, binding = 5) uniform sampler2D leftFace;
layout(set = 2, binding = 6) uniform sampler2D rightFace;
layout(set = 2, binding = 7) uniform sampler2D upFace;
layout(set = 2, binding = 8) uniform sampler2D downFace;
layout(set = 3, binding = 0) uniform Pass {
	mat4 InverseViewProjection;
	mat4 LightViewProjection;
	vec4 Planes, Target, Direction, Ambient, OutdoorAmbient, Direct;
	vec4 Eye, CameraDepth, FogColour, Fog, Shadow;
} pass;
layout(set = 3, binding = 1) uniform Fallback {
	vec4 Zenith, Horizon, Ground;
	vec4 AirColour, AirDecay, Air;
	vec4 Minimum, Maximum;
	vec4 CloudColourExtinction, CloudPattern;
	vec4 Sun, Direct, Ambient;
	uvec4 Modes;
} fallback;
const float PI = 3.14159265358979323846;

uint Hash(uint value) {
	value ^= value >> 16; value *= 0x7feb352du;
	value ^= value >> 15; value *= 0x846ca68bu;
	return value ^ (value >> 16);
}
float Random(ivec3 cell) {
	// The small periodic lattice agrees at the bounded displacement wrap.
	uvec3 p = uvec3(cell) & uvec3(7);
	return float(Hash(p.x * 0x8da6b343u ^ p.y * 0xd8163841u ^ p.z * 0xcb1ab31fu ^ fallback.Modes.z)) / 4294967295.0;
}
float Noise(vec3 point) {
	ivec3 p = ivec3(floor(point));
	vec3 f = fract(point); f = f * f * (3.0 - 2.0 * f);
	float a = mix(mix(Random(p), Random(p + ivec3(1, 0, 0)), f.x),
		mix(Random(p + ivec3(0, 1, 0)), Random(p + ivec3(1, 1, 0)), f.x), f.y);
	float b = mix(mix(Random(p + ivec3(0, 0, 1)), Random(p + ivec3(1, 0, 1)), f.x),
		mix(Random(p + ivec3(0, 1, 1)), Random(p + ivec3(1, 1, 1)), f.x), f.y);
	return mix(a, b, f.z);
}
vec4 Face(vec3 d) {
	vec3 a = abs(d); vec2 uv; uint face;
	if (a.z >= a.x && a.z >= a.y) {
		face = d.z >= 0 ? 0u : 1u;
		uv = vec2(d.z >= 0 ? d.x : -d.x, -d.y) / max(a.z, 1e-6) * .5 + .5;
		if ((fallback.Modes.y & (1u << face)) == 0u) return vec4(0);
		return vec4(d.z >= 0 ? texture(frontFace, uv).rgb : texture(backFace, uv).rgb, 1);
	}
	if (a.x >= a.y) {
		face = d.x >= 0 ? 2u : 3u;
		uv = vec2(d.x >= 0 ? -d.z : d.z, -d.y) / max(a.x, 1e-6) * .5 + .5;
		if ((fallback.Modes.y & (1u << face)) == 0u) return vec4(0);
		return vec4(d.x >= 0 ? texture(leftFace, uv).rgb : texture(rightFace, uv).rgb, 1);
	}
	face = d.y >= 0 ? 4u : 5u;
	uv = vec2(d.x, d.y >= 0 ? d.z : -d.z) / max(a.y, 1e-6) * .5 + .5;
	if ((fallback.Modes.y & (1u << face)) == 0u) return vec4(0);
	return vec4(d.y >= 0 ? texture(upFace, uv).rgb : texture(downFace, uv).rgb, 1);
}
vec3 Sky(vec3 direction) {
	if ((fallback.Modes.x & 1u) != 0u) {
		vec2 uv = vec2(atan(direction.x, direction.z) / (2 * PI) + .5,
			asin(clamp(direction.y, -1, 1)) / -PI + .5);
		return texture(environmentImage, uv).rgb;
	}
	vec3 colour = vec3(0);
	if ((fallback.Modes.x & 64u) != 0u) {
		colour = direction.y < 0 ? mix(fallback.Ground.rgb, fallback.Horizon.rgb, smoothstep(-.4, 0, direction.y))
			: mix(fallback.Horizon.rgb, fallback.Zenith.rgb, smoothstep(-.08, .45, direction.y));
		uint style = fallback.Modes.w & 255u;
		if (style == 1u) colour *= vec3(.85, .36, .22);
		else if (style == 2u) colour *= vec3(.012, .02, .055);
		else if (style == 3u) colour = mix(vec3(.015, .006, .04), vec3(.5, .06, .75), Noise(direction * 7));
		else if (style == 4u) colour = floor(colour * 8) / 8;
		colour += fallback.Direct.rgb * smoothstep(1 - max(fallback.Horizon.a, .001), 1, dot(direction, -fallback.Sun.xyz));
		if ((fallback.Modes.x & 16u) != 0u) { vec4 face = Face(direction); colour = mix(colour, face.rgb, face.a); }
	}
	if ((fallback.Modes.x & 32u) != 0u) {
		float horizon = exp(-abs(direction.y + fallback.Air.y * .12) * 5);
		float thickness = clamp(fallback.Air.x * (.28 + horizon), 0, 1);
		vec3 scatter = fallback.AirColour.rgb * (.35 + horizon * fallback.Air.w * .12);
		uint style = (fallback.Modes.w >> 16) & 255u;
		if (style == 1u) thickness *= .35;
		else if (style == 2u) scatter *= vec3(1, .45, .18);
		else if (style == 3u) scatter *= vec3(.4, .85, 1);
		colour = mix(colour * exp(-fallback.AirDecay.rgb * thickness), scatter, thickness);
		float glare = pow(max(dot(direction, -fallback.Sun.xyz), 0), 64 / max(fallback.Air.z, .25));
		colour += fallback.Direct.rgb * fallback.AirColour.rgb * glare * fallback.Air.z;
	}
	return colour;
}
bool Interval(vec3 origin, vec3 direction, float maximum, out float enter, out float leave) {
	enter = 0; leave = maximum;
	for (int axis = 0; axis < 3; ++axis) {
		if (abs(direction[axis]) < 1e-6) {
			if (origin[axis] < fallback.Minimum[axis] || origin[axis] > fallback.Maximum[axis]) return false;
		} else {
			float a = (fallback.Minimum[axis] - origin[axis]) / direction[axis];
			float b = (fallback.Maximum[axis] - origin[axis]) / direction[axis];
			enter = max(enter, min(a, b)); leave = min(leave, max(a, b));
		}
	}
	return enter < leave;
}
void main() {
	vec4 colour = texture(colourImage, inUv);
	bool linear = (fallback.Modes.x & 8u) != 0u;
	float depth = texture(depthImage, linear ? inUv : inUv * pass.Target.zw).r;
	vec2 clip = vec2(inUv.x * 2 - 1, 1 - inUv.y * 2);
	vec4 farPoint = pass.InverseViewProjection * vec4(clip, 1, 1);
	vec3 direction = normalize(farPoint.xyz / max(abs(farPoint.w), 1e-6) - pass.Eye.xyz);
	bool background = linear ? depth >= pass.Planes.y * .999 : depth >= .999999;
	float maximum = pass.Planes.y / max(dot(pass.CameraDepth.xyz, direction), 1e-6);
	if (!background) {
		if (linear) maximum = max(0, depth) / max(dot(pass.CameraDepth.xyz, direction), 1e-6);
		else {
			vec4 hit = pass.InverseViewProjection * vec4(clip, depth, 1);
			maximum = length(hit.xyz / max(abs(hit.w), 1e-6) - pass.Eye.xyz);
		}
	}
	if (background && (fallback.Modes.x & 2u) != 0u) colour.rgb = Sky(direction);
	float enter, leave;
	if ((fallback.Modes.x & 4u) != 0u && fallback.CloudPattern.x > 0 &&
		Interval(pass.Eye.xyz, direction, maximum, enter, leave)) {
		float stepMetres = (leave - enter) * .25;
		float density = 0;
		uint style = (fallback.Modes.w >> 8) & 255u;
		for (int sampleIndex = 0; sampleIndex < 4; ++sampleIndex) {
			vec3 point = pass.Eye.xyz + direction * (enter + (float(sampleIndex) + .5) * stepMetres);
			point.xz -= fallback.CloudPattern.zw;
			point /= max(fallback.CloudPattern.y, 1);
			if (style == 1u) point.y *= .18;
			else if (style == 3u) point = floor(point * 1.5) / 1.5;
			float noise = (Noise(point) * .5 + Noise(point * 2) * .25 + Noise(point * 4) * .125) / .875;
			density += smoothstep(1 - fallback.CloudPattern.x, 1, noise);
		}
		float opacity = 1 - exp(-density * stepMetres * fallback.CloudColourExtinction.w);
		float sunlight = max(-fallback.Sun.y, 0);
		vec3 litCloud = fallback.CloudColourExtinction.rgb * (fallback.Ambient.rgb + fallback.Direct.rgb * sunlight * .35);
		if (style == 2u) litCloud *= .55;
		colour.rgb = mix(colour.rgb, litCloud, clamp(opacity, 0, 1));
	}
	outColour = colour;
}
