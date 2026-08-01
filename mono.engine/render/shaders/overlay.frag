#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

// Fragment sampled textures are set 2 for SPIR-V.
layout(set = 2, binding = 0) uniform sampler2D overlayTexture;

void main() {
	// OverlayImage stores premultiplied alpha. The pipeline blends colour with
	// ONE / ONE_MINUS_SRC_ALPHA so alpha is applied exactly once.
	outColour = texture(overlayTexture, inUv);
}
