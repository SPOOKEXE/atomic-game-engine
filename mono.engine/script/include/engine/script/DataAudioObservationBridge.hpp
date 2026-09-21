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
	// Audio observation schema.
	inline constexpr std::string_view AUDIO_OBSERVATION_SCHEMA = "audio_observation/v1";
	// Max audio observation json bytes.
	inline constexpr size_t MAX_AUDIO_OBSERVATION_JSON_BYTES = 1'048'576;
	// Max audio observation string bytes.
	inline constexpr size_t MAX_AUDIO_OBSERVATION_STRING_BYTES = 4'096;
	// Max audio observation id bytes.
	inline constexpr size_t MAX_AUDIO_OBSERVATION_ID_BYTES = 512;
	// Max audio observation sources.
	inline constexpr size_t MAX_AUDIO_OBSERVATION_SOURCES = 4'096;
	// Max audio observation events.
	inline constexpr size_t MAX_AUDIO_OBSERVATION_EVENTS = 16'384;
	// Max audio observation chunks.
	inline constexpr size_t MAX_AUDIO_OBSERVATION_CHUNKS = 16'384;
	// Max audio observation missing.
	inline constexpr size_t MAX_AUDIO_OBSERVATION_MISSING = 16;
	// Max audio observation total string bytes.
	inline constexpr size_t MAX_AUDIO_OBSERVATION_TOTAL_STRING_BYTES = 262'144;

	// A serializable tick interval in audio_observation/v1.
	struct DataAudioObservationTickInterval {
		// Tick duration numerator, expressed in nanoseconds.
		uint64_t NumeratorNanoseconds = 0;
		// Divisor paired with NumeratorNanoseconds.
		uint64_t Denominator = 0;
	};

	// A serializable world clock in audio_observation/v1.
	struct DataAudioObservationWorldClock {
		// World tick for this record.
		uint64_t Tick = 0;
		// Elapsed world time in nanoseconds.
		uint64_t TimeNanoseconds = 0;
		// World incarnation that owns the sampled tick and elapsed time.
		uint64_t Epoch = 0;
		// Monotonic world state version sampled with this observation.
		std::string Version;
		// Rational fixed-tick duration used to advance the sampled world clock.
		DataAudioObservationTickInterval TickInterval;
	};

	// A serializable chunk in audio_observation/v1.
	struct DataAudioObservationChunk {
		// SHA-256 digest of the associated payload.
		std::string Sha256;
		// Inclusive byte-range start.
		uint64_t ByteBegin = 0;
		// Exclusive byte-range end.
		uint64_t ByteEnd = 0;
	};

	// A serializable waveform in audio_observation/v1.
	struct DataAudioObservationWaveform {
		// Whether this capability is available.
		bool Available = false;
		// Host diagnostic present when Available is false.
		std::string UnavailableReason;
		// Digest-addressed resource identifier.
		std::string ResourceId;
		// SHA-256 digest of the associated payload.
		std::string Sha256;
		// Total digest-addressed waveform payload length in bytes.
		uint64_t ByteLength = 0;
		// Ordered half-open byte ranges covering the digest-addressed waveform.
		std::vector<DataAudioObservationChunk> Chunks;
	};

	// A serializable source in audio_observation/v1.
	struct DataAudioObservationSource {
		// Stable source identity within this observation.
		std::string SourceId;
		// Schema discriminator identifying the observed source type.
		std::string Kind;
		// Unitless linear gain applied to this source at observation time.
		double Gain = 1.0;
		// Unitless stereo pan position applied to this source at observation time.
		double Pan = 0.0;
		// Whether the source was muted at the observed sample interval.
		bool Muted = false;
		// Whether the source was playing at the observed sample interval.
		bool Playing = false;
		// Whether playback wraps when this source reaches its end.
		bool Looping = false;
		// Whether the source still existed when the observation was assembled.
		bool Present = false;
	};

	// A serializable event in audio_observation/v1.
	struct DataAudioObservationEvent {
		// Stable event identity within this observation.
		std::string EventId;
		// Schema discriminator identifying the emitted event type.
		std::string Kind;
		// Source that emitted or received this event.
		std::string SourceId;
		// Optional second source identity associated with this event.
		std::string RelatedSourceId;
		// Audio sample at which the host requested the event.
		uint64_t RequestedSample = 0;
		// Audio sample at which the host applied the event.
		uint64_t AppliedSample = 0;
		// Event displacement from RequestedSample, measured in audio frames.
		uint64_t SampleOffsetFrames = 0;
		// Host timing classification explaining requested versus applied sample.
		std::string Timing;
		// Optional provenance describing why a finishing event occurred.
		std::string FinishProvenance;
	};

	// A serializable missingness in audio_observation/v1.
	struct DataAudioObservationMissingness {
		// Schema field whose value could not be observed.
		std::string Field;
		// Reason the value is unavailable.
		std::string Reason;
	};

	// Field names and nullability match datafactories-docs/audio_observation.py.
	// Empty RelatedSourceId and FinishProvenance encode JSON null.
	struct DataAudioObservation {
		// Host-generated identity for this immutable observation record.
		std::string ObservationId;
		// Copied clock identifying the world state sampled by this observation.
		DataAudioObservationWorldClock World;
		// Inclusive audio sample offset.
		uint64_t SampleBegin = 0;
		// Exclusive audio sample offset.
		uint64_t SampleEnd = 0;
		// Audio sample rate in hertz.
		uint64_t SampleRateHz = 0;
		// Requested capture channels.
		uint64_t Channels = 0;
		// Speaker or channel ordering used by the waveform payload.
		std::string ChannelLayout;
		// Optional digest-addressed waveform descriptor for this sample interval.
		DataAudioObservationWaveform Waveform;
		// Sources observed during the half-open sample interval.
		std::vector<DataAudioObservationSource> Sources;
		// Events applied during the half-open sample interval.
		std::vector<DataAudioObservationEvent> Events;
		// Fields intentionally unavailable from this host observation.
		std::vector<DataAudioObservationMissingness> Missing;
	};

	// A serializable bridge capabilities in audio_observation/v1.
	struct DataAudioObservationBridgeCapabilities {
		// Whether this capability is available.
		bool Available = false;
		// Human-readable diagnostic.
		std::string Detail;
		// Largest waveform read range this bridge accepts, in audio frames.
		uint64_t MaximumFrames = 0;
	};

	// A serializable bridge result in audio_observation/v1.
	struct DataAudioObservationBridgeResult {
		// Machine-readable operation status.
		std::string Status = "unavailable";
		// Human-readable diagnostic.
		std::string Detail;
		// Completed observation when Status indicates success.
		DataAudioObservation Observation;
	};

	// Validates every invariant that the v1 Python record validates before its
	// canonical JSON preflight. No bytes cross this boundary.
	bool ValidateDataAudioObservation(const DataAudioObservation &observation, std::string &failure);
	// Validates a bounded UTF-8 schema string, optionally requiring content.
	bool IsDataAudioObservationText(std::string_view value, size_t limit, bool nonempty = true);
	// Validates the SHA-256 digest spelling used by waveform descriptors.
	bool IsDataAudioObservationDigest(std::string_view value);

	// A serializable bridge in audio_observation/v1.
	class DataAudioObservationBridge {
	  public:
		virtual ~DataAudioObservationBridge() = default;
		// Reports availability and range limits of this installed audio observer.
		virtual DataAudioObservationBridgeCapabilities Capabilities() const = 0;
		// Captures one audio observation from the host.
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
