#pragma once

#include "NodeExecutors.hpp"
#include "SourceMirrorAnimator.hpp"

#include <array>
#include <optional>

namespace engine::imagegraph::detail {
	inline bool SourceMirrorPathSampled(const NodeContext &context, std::string_view port) {
		const auto index = SourceMirrorVectorIndex(port);
		return context.Authored.Type == "pc.mirror_polar" && index && context.MirrorPathSamples[*index];
	}
	inline bool SourceRepeatPathSampled(const NodeContext &context, std::string_view port) {
		const auto index = SourceConsumerVectorIndex(context.Authored.Type, port);
		return context.Authored.Type == "pc.path_repeat" && index && context.MirrorPathSamples[*index];
	}
	// Whole-input path getters run before processor rows, with the consumer's local raw animator.
	class SourceMirrorPathProjection {
	  public:
		explicit SourceMirrorPathProjection(NodeContext &context) : Context(context) {}
		~SourceMirrorPathProjection();
		bool Prepare();

	  private:
		NodeContext &Context;
		AllocationReservation Charge;
		std::array<Value, 7> Samples{};
		std::array<bool, 7> OriginalFlags{};
		std::vector<std::pair<std::string_view, const Value *>> OriginalViews;
		bool Installed = false;
	};
}
