#pragma once

// Bounded native PCM export with canonical RIFF headers and word-aligned data.
#include <engine/imagegraph/Document.hpp>

#include <cstddef>
#include <vector>

namespace engine::imagegraph {
	// PCM representation selected by the WAV File Out control.
	enum class WavExportFormat { Unsigned8, Signed16 };
	// Native sample conversion controls. Finite samples are clamped and rounded with ties to even.
	struct WavExportSettings {
		// Frames per second in the exported audio stream.
		uint32_t SampleRate = 44100;
		// Unsigned 8-bit or signed 16-bit little-endian PCM.
		WavExportFormat Format = WavExportFormat::Unsigned8;
		// Maps DataRange onto the complete selected PCM integer range before quantization.
		bool Remap = false;
		// Input endpoints used only when Remap is enabled.
		Vector2 DataRange{0, 1};
	};
	// A resolved destination and complete bytes, prepared without touching the filesystem.
	struct WavExport {
		// Resolved source path with a case-sensitive .wav suffix appended when absent.
		std::string Path;
		// Complete canonical PCM RIFF file, including required odd-data padding.
		std::vector<std::byte> Bytes;
	};
	// Requires explicit nested scalar channel rows of equal length. Failure preserves bytes.
	// The cap includes previous retained bytes and the complete replacement allocation.
	Status EncodeWavExport(
		const ArrayValue &channels,
		const WavExportSettings &settings,
		uint64_t maximumBytes,
		std::vector<std::byte> &bytes,
		Diagnostic &diagnostic
	);
	// Encodes borrowed, already-resolved source controls. The cap covers encoder-owned storage;
	// callers account for the borrowed controls. Failure preserves the previous export.
	Status PrepareResolvedWavExport(
		std::span<const AuthoredValue> inputs,
		uint64_t maximumBytes,
		WavExport &output,
		Diagnostic &diagnostic
	);
	// Captures linked and animated controls once through the central input snapshot. Failure preserves
	// export.
	Status PrepareWavExport(
		const Document &document,
		const Plan &plan,
		std::string_view nodeId,
		const EvaluationRequest &request,
		uint64_t maximumBytes,
		WavExport &output,
		Diagnostic &diagnostic
	);
}
