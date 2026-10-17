#version 450

// A compiled interface fragment.
//
// **One pipeline for a filled rectangle, an image and a glyph.** The atlas
// carries a solid white texel that an untextured quad samples, so a rectangle
// and a letter differ only in where they sample - not in which pipeline drew
// them. Two pipelines would be two places for the blend state to be set
// differently, which shows as panels at subtly the wrong opacity and nowhere
// else.

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec4 inColour;
layout(location = 2) in vec2 inCanvasPosition;

layout(location = 0) out vec4 outColour;

// Fragment sampled textures are set 2 for SPIR-V; uniform buffers are set 3.
layout(set = 2, binding = 0) uniform sampler2D interfaceTexture;

layout(set = 3, binding = 0) uniform Batch {
	// The collector-space scissor. Unlike a GPU scissor this remains correct
	// after a `SurfaceGui` plane is projected into perspective.
	vec4 Clip;

	// The compiler limits `UIMask` nesting to eight. Keeping that same fixed
	// limit here makes malformed lists unable to grow a fragment uniform block.
	vec4 MaskBounds[8];
	vec4 MaskData[8]; // x is the corner radius.
	vec4 MaskCount; // x is the number of active masks; y is layer opacity.
} batch;

bool insideRoundedRect(vec2 point, vec4 bounds, float radius) {
	if (point.x < bounds.x || point.y < bounds.y || point.x > bounds.z || point.y > bounds.w) {
		return false;
	}
	const float capped = min(max(radius, 0.0), min(bounds.z - bounds.x, bounds.w - bounds.y) * 0.5);
	if (capped <= 0.0) return true;
	const vec2 nearest = clamp(point, bounds.xy + vec2(capped), bounds.zw - vec2(capped));
	return dot(point - nearest, point - nearest) <= capped * capped;
}

void main() {
	if (inCanvasPosition.x < batch.Clip.x || inCanvasPosition.y < batch.Clip.y ||
		inCanvasPosition.x > batch.Clip.z || inCanvasPosition.y > batch.Clip.w) {
		discard;
	}
	for (int index = 0; index < int(batch.MaskCount.x); index++) {
		if (!insideRoundedRect(inCanvasPosition, batch.MaskBounds[index], batch.MaskData[index].x)) {
			discard;
		}
	}
	const vec4 sampled = texture(interfaceTexture, inUv);

	// **The atlas is coverage, not colour.** A glyph is one channel of alpha and
	// the colour is the vertex's, which is what lets one sheet serve every
	// colour of text in a frame. An image is RGBA and multiplies through.
	//
	// Both are the same expression because the atlas's white texel is
	// (1,1,1,1): a rectangle multiplies its tint by white and gets its tint,
	// and a glyph multiplies by (1,1,1,coverage) and gets its tint at that
	// coverage. Branching on which would be a divergent branch per fragment to
	// avoid a multiply by one.
	outColour = inColour * sampled;
	outColour.a *= batch.MaskCount.y;

	// **Discard rather than blend a zero.** The interface is drawn back to
	// front with no depth test, so a fully transparent fragment costs a blend
	// for nothing - and text is mostly transparent by area.
	if (outColour.a <= 0.0) {
		discard;
	}
}
