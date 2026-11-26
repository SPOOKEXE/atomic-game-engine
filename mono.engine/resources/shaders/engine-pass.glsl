// engine/pass: what an authored `vec4 Pass(PassInput i)` is written against.
// Its rgb is drawn over the lit image by its alpha on a composite pass, and
// kept for the next pass on any other. A `void Compute(PassInput i, ivec2
// pixel)` runs once per pixel of its output instead, and writes any pixel of it
// with `Store`.

#ifdef PASS_COMPUTE
#define PASS_SAMPLERS 0
#define PASS_VALUES 2
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D passOutput;
#else
#define PASS_SAMPLERS 2
#define PASS_VALUES 3
layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;
#endif

layout(set = PASS_SAMPLERS, binding = 0) uniform sampler2D passColour;
layout(set = PASS_SAMPLERS, binding = 1) uniform sampler2D passPrevious;
layout(set = PASS_SAMPLERS, binding = 2) uniform sampler2D passDepth;
layout(set = PASS_SAMPLERS, binding = 3) uniform sampler2D passNormal;
layout(set = PASS_SAMPLERS, binding = 4) uniform sampler2D passAlbedo;
layout(set = PASS_SAMPLERS, binding = 5) uniform sampler2D passEmissive;
// Named images from `#pragma image name`, in declaration order.
layout(set = PASS_SAMPLERS, binding = 6) uniform sampler2D passImage0;
layout(set = PASS_SAMPLERS, binding = 7) uniform sampler2D passImage1;
layout(set = PASS_SAMPLERS, binding = 8) uniform sampler2D passImage2;
layout(set = PASS_SAMPLERS, binding = 9) uniform sampler2D passImage3;

layout(set = PASS_VALUES, binding = 0) uniform PassValues {
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
	// What part of each named image was drawn: xy image 0, zw image 1, then 2 and 3.
	vec4 ImageScale0;
	vec4 ImageScale1;
	// xy the pixels this pass draws.
	vec4 Output;
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

#ifdef PASS_COMPUTE
// The pixels a compute pass covers.
ivec2 OutputSize() { return ivec2(lighting.Output.xy); }
// Writes one pixel of the output; outside it, nothing.
void Store(ivec2 pixel, vec4 value) {
	if (all(greaterThanEqual(pixel, ivec2(0))) && all(lessThan(pixel, OutputSize())))
		imageStore(passOutput, pixel, value);
}
#endif
