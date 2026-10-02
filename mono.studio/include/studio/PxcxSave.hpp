#pragma once

#include <engine/bake/Pxcx.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/FrameTime.hpp>

#include <filesystem>

namespace studio {
	// Validates all source edits before creating a temporary file, then atomically replaces the path.
	// The retained archive remains the authoring baseline so undo can restore opaque source records.
	[[nodiscard]] bool SavePxcxProjection(
		const std::filesystem::path &destination,
		const engine::bake::PxcxArchive &source,
		const engine::imagegraph::Document &authored,
		engine::imagegraph::FrameTime captureTime,
		engine::imagegraph::Diagnostic &diagnostic
	);
}
