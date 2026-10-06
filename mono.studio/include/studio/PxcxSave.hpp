#pragma once

#include <engine/bake/Pxcx.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/FrameTime.hpp>

#include <filesystem>
#include <optional>
#include <string_view>

namespace studio {
	// grug save a checked source Collection as .pxcc and optional .meta siblings. both are prepared
	// before publication; failed replacement restores old files or reports retained backup paths.
	// caller supplies source containing current edits. this leaves authoring and undo unchanged.
	[[nodiscard]] bool SavePxcxCollection(
		const std::filesystem::path &destination,
		const engine::bake::PxcxArchive &source,
		std::string_view collectionId,
		std::optional<std::string_view> managerJson,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	);
	// Validates all source edits before creating a temporary file, then atomically replaces the path.
	// The retained archive remains the authoring baseline so undo can restore opaque source records.
	[[nodiscard]] bool SavePxcxProjection(
		const std::filesystem::path &destination,
		const engine::bake::PxcxArchive &source,
		const engine::imagegraph::Document &authored,
		engine::imagegraph::FrameTime captureTime,
		engine::imagegraph::Diagnostic &diagnostic
	);
	// A completed preview belongs to one exact process-local authoring/input generation and output.
	struct PxcxPreviewIdentity {
		uint64_t DocumentRevision = 0;
		uint64_t InputRevision = 0;
		engine::imagegraph::Output Binding;
		engine::imagegraph::FrameTime Frame;
		uint8_t PlaybackObservation = 0;
		bool operator==(const PxcxPreviewIdentity &) const = default;
	};
	struct PxcxPreparedSavePreview {
		const engine::imagegraph::Image &Pixels;
		const PxcxPreviewIdentity &Completed;
		const PxcxPreviewIdentity &Current;
	};
	// Published-file state is separate from the original archive used for authoring and undo.
	// Keep this fact with its original authoring baseline; clear it when opening a different project.
	struct PxcxPublishedSave {
		engine::bake::PxcxArchive Archive;
		engine::imagegraph::Image ReferencePreview;
		std::optional<PxcxPreviewIdentity> ThumbnailIdentity;
	};
	// Optional preview explicitly replaces THMB. Otherwise retain the last published thumbnail,
	// or the original source thumbnail on the first save. Never evaluates the graph or waits for GPU.
	// Replaces published only after all encoding/readback succeeds and the file is atomically replaced.
	// maxBytes bounds retained archive/preview payload and staging per phase; existing projection
	// and bake vendor workspaces retain their own independent limits, not a full-process heap quota.
	[[nodiscard]] bool SavePxcxProjectionAndAdopt(
		const std::filesystem::path &destination,
		const engine::bake::PxcxArchive &source,
		const engine::imagegraph::Document &authored,
		engine::imagegraph::FrameTime captureTime,
		PxcxPublishedSave &published,
		const PxcxPreparedSavePreview *preview,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maxBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	);
}
