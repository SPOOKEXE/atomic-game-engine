#pragma once

#include <engine/imagegraph/SourceFont.hpp>
#include <engine/imagegraphfont/GraphFontHost.hpp>

#include <memory>
#include <string_view>

namespace engine::imagegraphfont {
	// Host-owned namespace, receipts and exact read capabilities; graph documents grant none.
	struct GraphFontConfiguration {
		imagegraph::SourceFontContext Context;
		std::vector<imagegraph::SourceFontObservation> Observations;
		std::vector<GraphFontFileGrant> ReadGrants;
	};
	std::optional<uint64_t> GraphFontConfigurationRetainedBytes(const GraphFontConfiguration &);
	// Validates complete candidates and retains one revision-bound process-local file provider.
	// Replace may run only after every synchronous evaluation borrowing this owner has returned.
	class GraphFontInputs {
	  public:
		GraphFontInputs() = default;
		GraphFontInputs(const GraphFontInputs &) = delete;
		GraphFontInputs &operator=(const GraphFontInputs &) = delete;
		GraphFontInputs(GraphFontInputs &&) = delete;
		GraphFontInputs &operator=(GraphFontInputs &&) = delete;
		bool Replace(
			const GraphFontConfiguration &,
			const assets::ContentPolicy &,
			uint64_t maximumBytes,
			imagegraph::Diagnostic &
		);
		// Copies a bounded immutable context for this held frame. The caller owns it through evaluation.
		bool Bind(
			bool playing,
			imagegraph::SourceFontContext &heldContext,
			imagegraph::EvaluationRequest &,
			uint64_t maximumBytes,
			imagegraph::Diagnostic &
		) const;
		const GraphFontConfiguration &Configuration() const {
			return Owned;
		}
		uint64_t Revision() const {
			return Version;
		}
		uint64_t RetainedBytes() const {
			return Bytes + (Provider ? Provider->RetainedBytes() : 0);
		}

	  private:
		GraphFontConfiguration Owned;
		std::unique_ptr<GraphFontHost> Provider;
		uint64_t Version = 0, Bytes = 0;
	};
	// Separate bounded artifact codec for runtime-only font values. Failure preserves the destination.
	bool ReadGraphFontConfiguration(
		std::string_view bytes, GraphFontConfiguration &, uint64_t maximumBytes, imagegraph::Diagnostic &
	);
	bool WriteGraphFontConfiguration(
		const GraphFontConfiguration &, std::string &, uint64_t maximumBytes, imagegraph::Diagnostic &
	);
}
