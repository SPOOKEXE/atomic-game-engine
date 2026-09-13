#pragma once

// A product-installed bridge from the VM-neutral data scene surface to an
// owner-thread audio capture. Script owns copied records, never a mixer, device,
// callback, or waveform allocation.
// @tier L9 · shared

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::script {
	inline constexpr std::string_view AUDIO_OBSERVATION_SCHEMA = "audio_observation/v1";
	inline constexpr size_t MAX_AUDIO_OBSERVATION_JSON_BYTES = 1'048'576;
	inline constexpr size_t MAX_AUDIO_OBSERVATION_STRING_BYTES = 4'096;
	inline constexpr size_t MAX_AUDIO_OBSERVATION_ID_BYTES = 512;
	inline constexpr size_t MAX_AUDIO_OBSERVATION_SOURCES = 4'096;
	inline constexpr size_t MAX_AUDIO_OBSERVATION_EVENTS = 16'384;
	inline constexpr size_t MAX_AUDIO_OBSERVATION_CHUNKS = 16'384;
	inline constexpr size_t MAX_AUDIO_OBSERVATION_MISSING = 16;
	inline constexpr size_t MAX_AUDIO_OBSERVATION_TOTAL_STRING_BYTES = 262'144;

	struct DataAudioObservationTickInterval {
		uint64_t NumeratorNanoseconds = 0;
		uint64_t Denominator = 0;
	};

	struct DataAudioObservationWorldClock {
		uint64_t Tick = 0;
		uint64_t TimeNanoseconds = 0;
		uint64_t Epoch = 0;
		std::string Version;
		DataAudioObservationTickInterval TickInterval;
	};

	struct DataAudioObservationChunk {
		std::string Sha256;
		uint64_t ByteBegin = 0;
		uint64_t ByteEnd = 0;
	};

	struct DataAudioObservationWaveform {
		bool Available = false;
		std::string UnavailableReason;
		std::string ResourceId;
		std::string Sha256;
		uint64_t ByteLength = 0;
		std::vector<DataAudioObservationChunk> Chunks;
	};

	struct DataAudioObservationSource {
		std::string SourceId;
		std::string Kind;
		double Gain = 1.0;
		double Pan = 0.0;
		bool Muted = false;
		bool Playing = false;
		bool Looping = false;
		bool Present = false;
	};

	struct DataAudioObservationEvent {
		std::string EventId;
		std::string Kind;
		std::string SourceId;
		std::string RelatedSourceId;
		uint64_t RequestedSample = 0;
		uint64_t AppliedSample = 0;
		uint64_t SampleOffsetFrames = 0;
		std::string Timing;
		std::string FinishProvenance;
	};

	struct DataAudioObservationMissingness {
		std::string Field;
		std::string Reason;
	};

	// Field names and nullability match datafactories-docs/audio_observation.py.
	// Empty RelatedSourceId and FinishProvenance encode JSON null.
	struct DataAudioObservation {
		std::string ObservationId;
		DataAudioObservationWorldClock World;
		uint64_t SampleBegin = 0;
		uint64_t SampleEnd = 0;
		uint64_t SampleRateHz = 0;
		uint64_t Channels = 0;
		std::string ChannelLayout;
		DataAudioObservationWaveform Waveform;
		std::vector<DataAudioObservationSource> Sources;
		std::vector<DataAudioObservationEvent> Events;
		std::vector<DataAudioObservationMissingness> Missing;
	};

	struct DataAudioObservationBridgeCapabilities {
		bool Available = false;
		std::string Detail;
		uint64_t MaximumFrames = 0;
	};

	struct DataAudioObservationBridgeResult {
		std::string Status = "unavailable";
		std::string Detail;
		DataAudioObservation Observation;
	};

	// Validates every invariant that the v1 Python record validates before its
	// canonical JSON preflight. No bytes cross this boundary.
	bool ValidateDataAudioObservation(const DataAudioObservation &observation, std::string &failure);
	bool IsDataAudioObservationText(std::string_view value, size_t limit, bool nonempty = true);
	bool IsDataAudioObservationDigest(std::string_view value);

	class DataAudioObservationBridge {
	  public:
		virtual ~DataAudioObservationBridge() = default;
		virtual DataAudioObservationBridgeCapabilities Capabilities() const = 0;
		virtual bool Capture(
			std::string_view instanceId, DataAudioObservationBridgeResult &result, std::string &detail
		) = 0;
		// Copies a caller-bounded range from an immutable digest-addressed waveform
		// resource. The bridge retains each resource and its byte ranges until its
		// host declares them expired. A successful read is exactly the requested
		// half-open range, so the caller can verify it against the supplied digest.
		virtual bool ReadWaveform(
			std::string_view instanceId,
			std::string_view resourceId,
			std::string_view sha256,
			uint64_t byteBegin,
			size_t maximumBytes,
			std::vector<std::byte> &bytes,
			std::string &detail
		) = 0;
	};
}
