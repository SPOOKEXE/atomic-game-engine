#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

// Fragment sampled textures are set 2 for SPIR-V.
layout(set = 2, binding = 0) uniform sampler2D overlayTexture;

void main() {
	// Straight alpha, matching what OverlayImage::Blend writes. The pipeline
	// blends SRC_ALPHA / ONE_MINUS_SRC_ALPHA.
	outColour = texture(overlayTexture, inUv);
}
