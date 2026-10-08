#pragma once

// Static image graph byte/texture adapters. Hosts own files and content policy.
// @tier L9 · shared

#include <engine/assets/Texture.hpp>
#include <engine/imagegraph/Document.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace engine::bake {
	// Refuses oversized encoded inputs before decoding; sources are static SDR images.
	// PNG, JPEG, BMP, GIF atlases and ordinary .atex data use existing decoders.
	// SVG needs an authored raster size and is not a source of this static adapter.
	bool DecodeImageGraphSource(
		std::string_view name,
		std::span<const std::byte> encoded,
		imagegraph::Image &out,
		std::string &failure
	);

	// Converts a valid graph result into an ordinary straight-alpha sRGB texture.
	// No graph identity reaches the output asset. Refusal preserves out.
	bool ImageGraphTexture(const imagegraph::Image &image, assets::TextureData &out, std::string &failure);

	// Compiles and evaluates a selected output using the host's decoded-source resolver.
	// This performs no filesystem or GPU work and preserves out on refusal.
	bool BakeImageGraph(
		const imagegraph::Document &document,
		std::string_view output,
		const imagegraph::SourceResolver &sources,
		assets::TextureData &out,
		imagegraph::Diagnostic &diagnostic
	);
}
