#version 450

// HDR down to something a display can show.
//
// **The pass that makes an HDR chain worth having.** Everything upstream works
// in `RGBA16` where a bright highlight is genuinely brighter than white; this
// is where that range is mapped into the 0-1 a screen takes. Without it the
// whole chain is just clipping, and there was no point in the wider format.
//
// **ACES, in its fitted form.** The full ACES transform is a pair of matrices
// and a rational curve; Narkowicz's fit is five constants and is within a
// fraction of a percent over the range that matters. A Reinhard curve is
// cheaper still and desaturates highlights in a way that reads as washed out —
// the thing people mean when they say a renderer looks flat.
//
// **Only `colour` is sampled, though the catalogue offers three more.** Bloom,
// exposure and a LUT are all optional inputs, and an optional input that is not
// wired packs no binding — so a shader sampling them reads whatever landed in
// the slot when they are absent. A chain with bloom to composite names its own
// shader; see `mix.frag`, which makes the same call for the same reason.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D colourImage;

vec3 Aces(vec3 x) {
	const float a = 2.51;
	const float b = 0.03;
	const float c = 2.43;
	const float d = 0.59;
	const float e = 0.14;
	return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
	vec4 source = texture(colourImage, inUv);

	vec3 mapped = Aces(max(source.rgb, vec3(0.0)));

	// **Encoded to sRGB here, because the target is `LDR` and not an sRGB
	// format.** A linear value written into a plain 8-bit target and shown
	// directly is the classic too-dark image; the render pass does no conversion
	// of its own, so the shader is the only place this can happen.
	outColour = vec4(pow(mapped, vec3(1.0 / 2.2)), source.a);
}
