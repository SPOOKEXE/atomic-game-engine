#pragma once

#include <engine/assets/ContentPolicy.hpp>
#include <engine/imagegraph/SourceFont.hpp>

#include <filesystem>

namespace engine::imagegraphfont {
	struct GraphFontFileGrant {
		std::string NodeId;
		std::filesystem::path File;
		bool Write = false;
		std::string Resource{};
	};
	// Exact file grants provide glyph observations without discovering ambient font families.
	class GraphFontHost final : public imagegraph::SourceFontProvider {
	  public:
		GraphFontHost(
			std::span<const GraphFontFileGrant> grants,
			assets::ContentPolicy policy,
			uint64_t maximumBytes = imagegraph::Limits::MaximumEvaluationBytes
		);
		bool Observe(
			const imagegraph::SourceFontRequest &,
			uint64_t maximumOperationBytes,
			imagegraph::SourceFontObservation &,
			std::string &failure
		) override;

		uint64_t RetainedBytes() const override {
			return GrantBytes + CacheBytes;
		}

	  private:
		struct FontGrant {
			std::string NodeId, Path, Resource;
			bool Write = false;
		};
		struct CachedFont {
			std::string Path;
			uint32_t PixelSize = 0;
			bool Antialias = false, SignedDistanceField = false;
			std::vector<std::byte> Bytes;
		};
		static constexpr size_t MaximumCachedFonts = 64;
		static constexpr uint64_t MaximumCacheBytes = 16u * 1024u * 1024u;
		std::vector<CachedFont> Cache;
		std::vector<FontGrant> Grants;
		assets::ContentPolicy Policy;
		uint64_t GrantBytes = 0, CacheBytes = 0;
	};
}
