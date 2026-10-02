#pragma once

// Source WAV preview decisions contain no device, filesystem or wall-clock state.
#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph {
	struct WavPreviewControls {
		std::string SourceId;
		bool Play = true;
		double Gain = .5;
		double ShiftSeconds = 0;
		uint32_t SampleRate = 0;
		size_t Frames = 0;
	};
	struct WavPreviewTimeline {
		bool Playing = false;
		double Frame = 0;
		uint64_t FirstFrame = 0;
		double FramesPerSecond = 30;
	};
	struct WavPreviewObservation {
		bool Known = false;
		bool Playing = false;
	};
	// Stop precedes Start when the selected first frame restarts an active preview.
	struct WavPreviewIntent {
		bool Stop = false;
		bool Start = false;
		double CursorFrames = 0;
		double Gain = .5;
	};
	// Uses the core timeline and linked-input resolver, without materializing WAV output samples.
	Status ResolveWavPreviewControls(
		const Document &document,
		const std::string &nodeId,
		const EvaluationRequest &request,
		WavPreviewControls &controls,
		Diagnostic &diagnostic
	);
	Status MakeWavPreviewIntent(
		const WavPreviewControls &controls,
		const WavPreviewTimeline &timeline,
		const WavPreviewObservation &observed,
		WavPreviewIntent &intent,
		Diagnostic &diagnostic
	);
	// Channel zero uses source PCM16 quantization and /2 attenuation, independently of Mono.
	Status BuildWavPreviewSamples(
		const AudioBit &clip, uint64_t byteBudget, std::vector<float> &samples, Diagnostic &diagnostic
	);
	Status WavPreviewSyncFrames(
		const WavPreviewControls &controls, double framesPerSecond, uint64_t &frames, Diagnostic &diagnostic
	);
}
