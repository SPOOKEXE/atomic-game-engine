#version 450

// A compiled interface fragment.
//
// **One pipeline for a filled rectangle, an image and a glyph.** The atlas
// carries a solid white texel that an untextured quad samples, so a rectangle
// and a letter differ only in where they sample — not in which pipeline drew
// them. Two pipelines would be two places for the blend state to be set
// differently, which shows as panels at subtly the wrong opacity and nowhere
// else.

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec4 inColour;

layout(location = 0) out vec4 outColour;

// Fragment sampled textures are set 2 for SPIR-V; uniform buffers are set 3.
layout(set = 2, binding = 0) uniform sampler2D interfaceTexture;

layout(set = 3, binding = 0) uniform Flipbook {
	// Where the current animation cell sits: x the scale, yz the offset.
	//
	// **The identity for everything that is not a sheet**, which is a rectangle,
	// a glyph and every still image — so this is applied unconditionally rather
	// than behind a branch. `render::FlipbookCell` carries why a transform
	// rather than a cell index.
	vec4 Cell;
} flipbook;

void main() {
	const vec2 cellUv = inUv * flipbook.Cell.x + flipbook.Cell.yz;
	const vec4 sampled = texture(interfaceTexture, cellUv);

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

	// **Discard rather than blend a zero.** The interface is drawn back to
	// front with no depth test, so a fully transparent fragment costs a blend
	// for nothing — and text is mostly transparent by area.
	if (outColour.a <= 0.0) {
		discard;
	}
}
