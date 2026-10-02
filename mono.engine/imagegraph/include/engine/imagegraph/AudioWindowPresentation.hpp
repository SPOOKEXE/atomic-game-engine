#pragma once
#include <engine/imagegraph/Document.hpp>
namespace engine::imagegraph {
	// One resolved Audio Window observation; no borrowed audio pointer escapes capture.
	struct AudioWindowPresentation {
		// Owned native waveform points: normalized sample position and channel-zero amplitude.
		std::vector<Vector2> Points;
		// Raw source cursor normalized by packet count; drawing clamps it to [0,1].
		double Cursor = 0;
		// Selected exclusive interval start normalized by packet count.
		double Start = 0;
		// Selected exclusive interval end normalized by packet count.
		double End = 0;
		// Source planar channel count.
		size_t Channels = 0;
		// Source packets per channel.
		size_t Packets = 0;
		// Source sample rate in Hz.
		double SampleRate = 0;
		// Native geometry cache fingerprint, including channel zero and source shape.
		uint64_t GeometryKey = 0;
	};
	// Captures actual linked/animated inputs once; shares executor readers and bounds.
	// Native visualization uses at most 320 evenly spaced integer samples, not source raster parity.
	// Failure preserves output. Cap admits old points, input snapshot, view scratch and candidate capacity.
	Status ResolveAudioWindowPresentation(
		const Document &document,
		const Plan &plan,
		std::string_view nodeId,
		const EvaluationRequest &request,
		uint64_t byteBudget,
		AudioWindowPresentation &output,
		Diagnostic &diagnostic
	);
}
