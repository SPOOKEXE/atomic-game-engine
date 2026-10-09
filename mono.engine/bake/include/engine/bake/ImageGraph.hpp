#pragma once

// image graph source, export and live cook bytes. hosts own files and content policy.
// @tier L9 · shared

#include <engine/assets/Texture.hpp>
#include <engine/imagegraph/Document.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

	// grug keeps colour bytes encoded and data bytes raw. refusal keeps old image.
	bool DecodeImageGraphSourceTyped(
		const imagegraph::Source &source,
		std::span<const std::byte> encoded,
		imagegraph::Image &out,
		std::string &failure
	);

	// one exact source texture named by its canonical bytes.
	struct CookedImageGraphSource {
		// signed runtime texture name, never an authored filesystem path.
		std::string Name;
		// exact graph pixels and sampling space, with no resize or mip chain.
		assets::TextureData Texture;
	};
	// complete live asset closure. grug publishes dependencies before graph.
	struct CookedImageGraph {
		// graph's signed runtime asset name.
		std::string Name;
		// checked graph with source paths rewritten to runtime names.
		imagegraph::Document Graph;
		// canonical runtime graph JSON.
		std::string Text;
		// unique normalized source textures in deterministic name order.
		std::vector<CookedImageGraphSource> Sources;
	};
	// grug normalizes every authored source and bound path default without resizing.
	// hosts own source lookup. refusal keeps out. runtime path overrides name signed textures.
	bool CookImageGraph(
		const imagegraph::Document &document,
		std::string_view graphAssetName,
		const imagegraph::TypedSourceResolver &sources,
		CookedImageGraph &out,
		imagegraph::Diagnostic &diagnostic
	);
	// static export with colour/data-aware source lookup. refusal keeps out.
	bool BakeImageGraphTyped(
		const imagegraph::Document &document,
		std::string_view output,
		const imagegraph::TypedSourceResolver &sources,
		assets::TextureData &out,
		imagegraph::Diagnostic &diagnostic
	);

	// grug keeps straight alpha and the graph output sampling space in an ordinary texture.
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
