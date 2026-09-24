#pragma once

// Bounded text codec for deterministic mono audio frames supplied to evaluation.

#include <engine/imagegraph/Document.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::imagegraph {

	// Checks IDs, ticks, duplicate keys, sample finiteness and aggregate limits.
	Status ValidateAudioCaptureFrames(std::span<const AudioCaptureFrame> frames, Diagnostic &diagnostic);

	// Parses a bounded `audio-capture 1` or `audio-capture 2` document without changing output on failure.
	Status
	ReadAudioCapture(std::string_view text, std::vector<AudioCaptureFrame> &frames, Diagnostic &diagnostic);

	// Writes a stable, locale-independent capture document, using v2 when any frame carries a sample rate.
	Status
	WriteAudioCapture(std::span<const AudioCaptureFrame> frames, std::string &text, Diagnostic &diagnostic);
}
