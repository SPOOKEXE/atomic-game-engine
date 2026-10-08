#pragma once

#include <string_view>

namespace engine::imagegraph {
	enum class SourceCommonStepKind { Unsupported, NodeDataCommon, CollectionOverride, CacheOverride };
	enum class SourceCommonWrapperKind { Unsupported, Full, Lite, Empty };
	struct Node;
	// grug exact annotations have empty source callbacks, without a pixel executor or canvas layout claim.
	bool SourceCommonEmptyOwnerMatches(const Node &node, std::string_view sourceType) noexcept;
	struct SourceCommonDispatch {
		SourceCommonStepKind Step = SourceCommonStepKind::Unsupported;
		SourceCommonWrapperKind Wrapper = SourceCommonWrapperKind::Unsupported;
		bool operator==(const SourceCommonDispatch &) const = default;
	};
	// Capability is attested by the actual core/host registry, never durable flags. This immutable
	// pinned-source profile neither executes nor infers callback completion. Collection exposes its
	// structural dispatch without attesting a native callback executor. Empty admits only the two
	// exact annotation overrides and does not attest a pixel executor.
	SourceCommonDispatch
	SourceCommonDispatchProfile(std::string_view sourceType, bool nativeExecutable = false) noexcept;
}
