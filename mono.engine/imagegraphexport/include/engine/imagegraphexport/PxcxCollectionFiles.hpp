#pragma once

#include <engine/imagegraphio/PxcxAppend.hpp>

#include <span>

namespace engine::imagegraphexport {
	// grug caller supplies completed immutable pixels. keep full size; convert to straight RGBA8.
	// no render or files here. refusal preserves prior output; cap includes input and codec backing.
	[[nodiscard]] bool WritePxcxCollectionPreview(
		const imagegraph::Image &preview,
		std::vector<std::byte> &out,
		imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = imagegraph::Limits::MaximumEvaluationBytes
	);
	// grug package prepared source text and optional encoded PNG under one bounded basename.
	// stored ZIP entries preserve decoded file bytes; encoder bytes need no GameMaker parity claim.
	// no paths opened. refusal preserves prior output; cap includes retained inputs and codec backing.
	[[nodiscard]] bool WritePxcxCollectionPackage(
		std::string_view baseName,
		const imagegraphio::PxcxCollectionSave &collection,
		std::span<const std::byte> previewPng,
		std::vector<std::byte> &out,
		imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = imagegraph::Limits::MaximumEvaluationBytes
	);
}
