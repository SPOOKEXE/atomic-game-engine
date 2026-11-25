#pragma once

// Image graph source bindings and the per-evaluation statistics the renderer reports.

#include <engine/assets/Texture.hpp>
#include <engine/core/Name.hpp>
#include <engine/imagegraph/Document.hpp>

#include <cstdint>
#include <string>

namespace engine::render {
	// Exact resident source for one authored relative path. No owner fallback occurs.
	struct ImageGraphSourceBinding {
		// Authored source path, matched together with Interpretation.
		std::string Path;
		// Ordinary resident texture name and its exact residency namespace.
		core::Name Texture;

		// Content namespace used to resolve this exact resident texture.
		core::Name Owner;
		// Colour imports encoded colour; Data preserves normalized numeric channels.
		imagegraph::SourceInterpretation Interpretation = imagegraph::SourceInterpretation::Colour;
		// The texture's name identifies its pixels, so it need only be resident while an
		// evaluation reads it. Width, Height and Format describe it while it is not;
		// `Renderer::ImageGraphNeededSource` says when to provide it.
		bool ContentAddressed = false;
		// Declared extent of a content-addressed texture.
		uint32_t Width = 0, Height = 0;
		// Declared format of a content-addressed texture.
		assets::TextureFormat Format = assets::TextureFormat::RGBA8;
	};

	// Accepted GPU update, unchanged resident output, last-good output retained, or work
	// spread over later calls: call again each frame until it settles.
	enum class ImageGraphEvaluation : uint8_t { Updated, Reused, Refused, Pending };

	// Cumulative traffic and the latest completed GPU evaluation measurement.
	struct ImageGraphStatistics {
		// Submitted evaluations and unchanged calls since renderer initialization.
		uint64_t Evaluations = 0;
		// Evaluations that reused unchanged resident output.
		uint64_t CacheHits = 0;
		// Actual recorded compute work and successfully submitted command buffers.
		uint64_t ComputeDispatches = 0;
		// Successfully submitted buffers used for graph evaluation.
		uint64_t CommandBuffers = 0;
		// Logical graph texture allocations, publications and current residency.
		uint64_t AllocatedBytes = 0;
		// Logical bytes copied while publishing graph output.
		uint64_t CopiedBytes = 0;
		// Logical bytes currently resident for graph textures.
		uint64_t ResidentBytes = 0;
		// Sequence of the latest completed nonblocking device measurement.
		uint64_t GpuTimingSequence = 0;
		// Latest changed evaluation recording cost and completed device duration.
		double CpuRecordingMicroseconds = 0;
		// Device duration from the latest completed evaluation measurement.
		double GpuMicroseconds = 0;
		// Device timestamp support, independent of whether a result has completed.
		bool HasGpuTimings = false;
	};
}
