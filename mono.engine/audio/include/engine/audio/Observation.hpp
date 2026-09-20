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
		// Stable scene-supplied source identifier.
		std::string_view SourceId;
		// Process-local mixer node resolved from the source id.
		NodeId Node;
	};

	// Bounds for a single copied observation.
	struct AudioObservationLimits {
		// Maximum waveform frames copied into one observation.
		size_t MaximumFrames = DEFAULT_BLOCK_FRAMES;
		// Maximum scene source records copied into one observation.
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
		// Whether this feature was available for the copied block.
		bool Available = false;
		// Collector method that produced the feature.
		std::string Method;
		// Reason the feature is unavailable when Available is false.
		std::string Reason;
	};

	// Current state for one stable scene-owned source.
	struct AudioSourceObservation {
		// Stable scene identifier of the observed source.
		std::string SourceId;
		// Scene audio class reported for the source.
		std::string Kind;
		// Linear mixer gain before output clipping.
		float Gain = 1.0f;
		// Equal-power stereo pan position.
		float Pan = 0.0f;
		// Whether the source is muted by the mixer.
		bool Muted = false;
		// Whether the source has active playback.
		bool Playing = false;
		// Whether playback restarts at the end of the source.
		bool Looping = false;
		// Whether a mixer node currently represents this source.
		bool Present = false;
		// Whether Placement contains an authored emitter pose.
		bool HasEmitterPlacement = false;
		// Authored emitter pose when HasEmitterPlacement is true.
		EmitterPlacement Placement;
		// Distance from listener to emitter in metres.
		float ListenerDistance = 0.0f;
		// Stereo attenuation after spatialisation.
		StereoGain SpatialGain;
	};

	// One graph command aligned to both the requested and actual sample clocks.
	struct AudioEventObservation {
		// Mixer command kind.
		std::string Kind;
		// Stable id of the command's primary source.
		std::string SourceId;
		// Stable id of the command's secondary source, if any.
		std::string RelatedSourceId;
		// Whether SourceId resolved to a scene source.
		bool SourceIdentified = false;
		// Whether RelatedSourceId resolved to a scene source.
		bool RelatedSourceIdentified = false;
		// Sample clock position requested by the game tick.
		uint64_t RequestedSample = 0;
		// Sample clock position where the mixer applied the command.
		uint64_t AppliedSample = 0;
		// Frame offset within the copied mixer block.
		size_t OffsetFrames = 0;
		// Timing classification for the requested and applied samples.
		std::string Timing;
	};

	// Immutable copy of one mixer block and its synchronized labels.
	struct AudioObservation {
		// First sample clock position in the copied block.
		const uint64_t BeginSample;
		// One past the final sample clock position in the copied block.
		const uint64_t EndSample;
		// Audio samples per second used for the copied block.
		const uint32_t SampleRate;
		// Number of interleaved output channels.
		const uint16_t Channels;
		// Name of the copied output channel layout.
		const std::string ChannelLayout;
		// Name of the copied sample representation.
		const std::string SampleType;
		// Whether waveform samples are interleaved by channel.
		const bool Interleaved;
		// Immutable waveform samples from the copied block.
		const std::vector<float> Waveform;
		// Observed scene source states for the copied block.
		const std::vector<AudioSourceObservation> Sources;
		// Mixer commands applied during the copied block.
		const std::vector<AudioEventObservation> Events;
		// Listener pose used for spatialisation.
		const ListenerPose Listener;
		// Availability and origin of the waveform capture.
		const AudioFeatureProvenance WaveformProvenance;
		// Availability and origin of command timing data.
		const AudioFeatureProvenance TimingProvenance;
		// Availability and origin of attenuation data.
		const AudioFeatureProvenance AttenuationProvenance;
		// Availability and origin of occlusion data.
		const AudioFeatureProvenance OcclusionProvenance;
	};

	// Result of validating and copying an observation.
	struct AudioObservationResult {
		// Capture outcome, including any rejected input condition.
		AudioObservationStatus Status = AudioObservationStatus::Ready;
		// Immutable observation when Status is Ready.
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
