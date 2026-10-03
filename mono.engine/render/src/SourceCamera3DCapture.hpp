#pragma once
#include <engine/render/SourceCamera3D.hpp>
namespace engine::render::imagegraph {
	// Converts all completed device surfaces together; failure preserves the owned receipt.
	bool CompleteSourceCamera3DCapture(
		const SourceCamera3DRequest &,
		std::span<const std::span<const std::byte>> downloads,
		uint64_t maximumBytes,
		engine::imagegraph::HostNodeCapture &
	);
}
