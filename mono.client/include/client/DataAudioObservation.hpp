#pragma once

// Client-owned, immutable audio observation retention for data-factory mode.
// The host publishes after a null-device render, so no device callback or
// mixer graph crosses the control boundary.
// @tier client

#include <engine/audio/Observation.hpp>
#include <engine/script/DataAudioObservationBridge.hpp>
#include <engine/world/DataFactory.hpp>

#include <client/Sounds.hpp>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace client {
	// Answers one direct control request and publishes after a completed
	// lifecycle boundary. The observer receives the post-mutation clock, which
	// is the clock the following audio capture must retain.
	std::string AnswerDataFactoryControlLine(
		engine::world::DataFactorySession &session,
		std::string_view instanceId,
		const std::function<std::string()> &answer,
		const std::function<void(const engine::world::DataFactoryReply &)> &observe
	);

	// Data Audio Observation Host declaration.
	class DataAudioObservationHost final : public engine::script::DataAudioObservationBridge {
	  public:
		// Type used for Inspect.
		using Inspect = std::function<engine::world::DataFactoryReply(std::string_view)>;

		// Builds a bridge host that uses inspect to fence records to factory lifecycle revisions.
		explicit DataAudioObservationHost(Inspect inspect = {});

		// Copies one completed null-device block and its scene labels. Replacing
		// a lifecycle version drops all bytes from the previous version.
		bool Publish(
			const engine::world::DataFactoryReply &clock,
			const engine::audio::AudioMixer &mixer,
			const engine::audio::SampleBuffer &waveform,
			const engine::audio::MixReport &report,
			const std::vector<AudioObservationSourceBinding> &sources,
			std::string &detail,
			bool complete = true
		);

		// Reports whether this host can produce sample-aligned copied audio observations.
		engine::script::DataAudioObservationBridgeCapabilities Capabilities() const override;
		// Copies the current observation for instanceId into a bridge result.
		bool Capture(
			std::string_view instanceId,
			engine::script::DataAudioObservationBridgeResult &result,
			std::string &detail
		) override;
		bool ReadWaveform(
			std::string_view instanceId,
			std::string_view resourceId,
			std::string_view sha256,
			uint64_t byteBegin,
			size_t maximumBytes,
			std::vector<std::byte> &bytes,
			std::string &detail
		) override;

		// Discards cached audio observation state for one destroyed or reset instance.
		void Clear(std::string_view instanceId);
		// Drops cached records whose lifecycle revision differs from clock.
		void InvalidateUnless(const engine::world::DataFactoryReply &clock);

		// The caller owns the null mixer. This state converts completed fixed
		// simulation ticks to exact audio frames without consulting presentation.
		void ResetTickClock(const engine::world::DataFactoryReply &clock, uint32_t sampleRate);
		// Advances each retained observation by exact sample frames elapsed between ticks.
		std::vector<size_t> AdvanceFrames(const engine::world::DataFactoryReply &clock, uint32_t sampleRate);

	  private:
		struct Retained {
			uint64_t Epoch = 0;
			uint64_t Version = 0;
			engine::script::DataAudioObservation Observation;
			std::vector<std::byte> Waveform;
		};
		struct SceneRecords {
			uint64_t Epoch = 0;
			uint64_t Version = 0;
			engine::script::DataAudioObservation Latest;
			std::deque<Retained> Artifacts;
			size_t Bytes = 0;
			std::vector<engine::script::DataAudioObservation> Pending;
			std::vector<std::byte> PendingWaveform;
		};

		mutable std::mutex Mutex;
		Inspect InspectClock;
		std::unordered_map<std::string, SceneRecords> Records;
		uint64_t TickEpoch = 0;
		uint64_t Tick = 0;
		uint64_t TickRemainder = 0;
		bool TickClockReady = false;
	};
}
