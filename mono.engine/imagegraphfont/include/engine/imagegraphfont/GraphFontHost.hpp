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
		GraphFontHost(std::span<const GraphFontFileGrant> grants, assets::ContentPolicy policy);
		bool Observe(
			const imagegraph::SourceFontRequest &,
			uint64_t maximumOperationBytes,
			imagegraph::SourceFontObservation &,
			std::string &failure
		) override;

		uint64_t RetainedBytes() const {
			return GrantBytes;
		}

	  private:
		struct FontGrant {
			std::string NodeId, Path, Resource;
			bool Write = false;
		};
		std::vector<FontGrant> Grants;
		assets::ContentPolicy Policy;
		uint64_t GrantBytes = 0;
	};
}
