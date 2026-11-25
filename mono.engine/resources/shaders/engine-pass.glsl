// engine/pass: what an authored `vec4 Pass(PassInput i)` is written against.
// Its rgb is drawn over the lit image by its alpha on a composite pass, and
// kept for the next pass on any other.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D passColour;
layout(set = 2, binding = 1) uniform sampler2D passPrevious;
layout(set = 2, binding = 2) uniform sampler2D passDepth;
layout(set = 2, binding = 3) uniform sampler2D passNormal;
layout(set = 2, binding = 4) uniform sampler2D passAlbedo;
layout(set = 2, binding = 5) uniform sampler2D passEmissive;

layout(set = 3, binding = 0) uniform PassValues {
	// xy lit size in pixels, zw its inverse.
	vec4 Target;
	// xy what part of the previous pass's image was drawn, zw of the G-buffer.
	vec4 Scale;
	// x seconds, y pixels per world unit at depth 1, z far depth.
	vec4 View;
	vec4 Parameter0;
	vec4 Parameter1;
	vec4 Parameter2;
	vec4 Parameter3;
	vec4 ParameterState;
} lighting;

struct PassInput {
	vec2 UV;
	vec2 Texel;         // one lit pixel in UV
	float Depth;        // linear, world units
	float PixelsPerUnit; // at this depth
	float Time;
};

// The lit image when this pass began.
vec4 Colour(vec2 uv) { return texture(passColour, uv); }
// What the last non-composite pass returned.
vec4 Previous(vec2 uv) { return texture(passPrevious, uv * lighting.Scale.xy); }
float Depth(vec2 uv) { return texture(passDepth, uv).r; }
bool IsSky(vec2 uv) { return Depth(uv) >= lighting.View.z; }
vec3 Normal(vec2 uv) { return normalize(texture(passNormal, uv * lighting.Scale.zw).xyz * 2.0 - 1.0); }
vec3 Albedo(vec2 uv) { return texture(passAlbedo, uv * lighting.Scale.zw).rgb; }
// The `User` tag a surface with a Light wrote, or 0.
float User(vec2 uv) {
	float marker = texture(passNormal, uv * lighting.Scale.zw).a;
	return marker > 0.17 && marker < 0.5 ? texture(passEmissive, uv * lighting.Scale.zw).a : 0.0;
}
