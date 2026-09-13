#pragma once

// arch-waiver public-header: forward audio API. Data capture hosts copy this
// complete observation after an offline or owner-thread mix.

// A durable, sample-aligned view of one mixed audio block.
//
// The mixer keeps only a fixed-size command trace because its callback cannot
// allocate. `CaptureAudioObservation` runs after that callback on the owning
// thread and copies the waveform, stable source names, graph state and event
// timing into an immutable value suitable for a data-factory adapter.
//
// @tier L12 · client

#include <engine/audio/Mixer.hpp>
#include <engine/audio/Spatial.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::audio {

	// A stable source name supplied by the scene host and its process-local
	// mixer node. The numeric node never appears in the captured observation.
	struct AudioSourceBinding {
		std::string_view SourceId;
		NodeId Node;
	};

	// Bounds for a single copied observation.
	struct AudioObservationLimits {
		size_t MaximumFrames = DEFAULT_BLOCK_FRAMES;
		size_t MaximumSources = AudioGraph::MAXIMUM_NODES;
	};

	// Why an observation was or was not produced.
	//
	// @since v0.24
	enum class AudioObservationStatus : uint8_t {
		Ready,
		InvalidFormat,
		StaleMixReport,
		WaveformLimitExceeded,
		SourceLimitExceeded,
		InvalidSourceBinding,
		DuplicateSourceBinding,
	};

	// Whether a labelled audio feature exists, and where its value came from.
	struct AudioFeatureProvenance {
		bool Available = false;
		std::string Method;
		std::string Reason;
	};

	// Current state for one stable scene-owned source.
	struct AudioSourceObservation {
		std::string SourceId;
		std::string Kind;
		float Gain = 1.0f;
		float Pan = 0.0f;
		bool Muted = false;
		bool Playing = false;
		bool Looping = false;
		bool Present = false;
		bool HasEmitterPlacement = false;
		EmitterPlacement Placement;
		float ListenerDistance = 0.0f;
		StereoGain SpatialGain;
	};

	// One graph command aligned to both the requested and actual sample clocks.
	struct AudioEventObservation {
		std::string Kind;
		std::string SourceId;
		std::string RelatedSourceId;
		bool SourceIdentified = false;
		bool RelatedSourceIdentified = false;
		uint64_t RequestedSample = 0;
		uint64_t AppliedSample = 0;
		size_t OffsetFrames = 0;
		std::string Timing;
	};

	// Immutable copy of one mixer block and its synchronized labels.
	struct AudioObservation {
		const uint64_t BeginSample;
		const uint64_t EndSample;
		const uint32_t SampleRate;
		const uint16_t Channels;
		const std::string ChannelLayout;
		const std::string SampleType;
		const bool Interleaved;
		const std::vector<float> Waveform;
		const std::vector<AudioSourceObservation> Sources;
		const std::vector<AudioEventObservation> Events;
		const ListenerPose Listener;
		const AudioFeatureProvenance WaveformProvenance;
		const AudioFeatureProvenance TimingProvenance;
		const AudioFeatureProvenance AttenuationProvenance;
		const AudioFeatureProvenance OcclusionProvenance;
	};

	// Result of validating and copying an observation.
	struct AudioObservationResult {
		AudioObservationStatus Status = AudioObservationStatus::Ready;
		std::optional<AudioObservation> Value;
	};

	// Copies the most recently rendered block into an immutable observation.
	//
	// Call this only after `AudioMixer::Render` has returned and on the thread
	// that owns the mixer. It rejects an old report, ambiguous source bindings,
	// format mismatches and configured bounds rather than emitting partial data.
	AudioObservationResult CaptureAudioObservation(
		const AudioMixer &mixer,
		const SampleBuffer &waveform,
		const MixReport &report,
		std::span<const AudioSourceBinding> sources,
		AudioObservationLimits limits = {}
	);
}
