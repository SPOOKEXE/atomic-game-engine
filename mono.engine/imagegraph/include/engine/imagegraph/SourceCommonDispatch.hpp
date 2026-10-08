#pragma once

#include <string_view>

namespace engine::imagegraph {
	enum class SourceCommonStepKind { Unsupported, NodeDataCommon, CollectionOverride };
	enum class SourceCommonWrapperKind { Unsupported, Full, Lite };
	struct SourceCommonDispatch {
		SourceCommonStepKind Step = SourceCommonStepKind::Unsupported;
		SourceCommonWrapperKind Wrapper = SourceCommonWrapperKind::Unsupported;
		bool operator==(const SourceCommonDispatch &) const = default;
	};
	// Capability is attested by the actual core/host registry, never durable flags. This immutable
	// pinned-source profile neither executes nor infers callback completion. Collection exposes its
	// structural dispatch without attesting a native callback executor.
	SourceCommonDispatch
	SourceCommonDispatchProfile(std::string_view sourceType, bool nativeExecutable = false) noexcept;
}
