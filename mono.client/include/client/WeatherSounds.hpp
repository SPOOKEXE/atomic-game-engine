#pragma once

// Listener-local thunder uses accepted layer clocks and the mixer's sample deadlines.
// Mixer handles remain in this adapter; the ECS owns storm history and arrival queues.

#include <engine/audio/Mixer.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/weather/Events.hpp>

#include <vector>

namespace engine::ecs {
	class Store;
}

namespace client {
	class SoundCatalogue;
	// Prepares copied listener arrivals without opening an audio device.
	// Expired waveform arrivals remain consumed. Returns true for an active
	// accepted layer with an audible tail or thunder within the sample horizon.
	[[nodiscard]] bool WeatherThunderDueSoon(
		engine::ecs::Store &store, engine::core::Vector3 listener, double horizonSeconds = 0.25
	);
	// One active or scheduled sound and its accepted copied event identity.
	struct WeatherSoundVoice {
		// Provider is a local lookup handle, never a serialized event identity.
		engine::ecs::Entity Provider;
		// Stable event receipt guard and mixer resource binding.
		//@{
		engine::weather::StormEvent Event;
		engine::audio::NodeId Player;
		// Exact first output sample; late arrivals seek into their source waveform.
		uint64_t StartSample = 0;
		// Exact end of the remaining source tail, excluding any propagation delay.
		uint64_t EndSample = 0;
		// Retried until terminal commands are accepted, even if the old deadline passes meanwhile.
		bool Retiring = false;
		//@}
	};
	// Mixer ownership for one world. Refused queue writes remain retryable.
	struct WeatherSoundStage {
		// Bounded resource bindings, retired after their sound has completed.
		std::vector<WeatherSoundVoice> Voices;
		// Reusable deterministic thunder waveform, generated outside the callback.
		engine::audio::SoundRef Thunder;
		// Counters report actual accepted starts and queue refusals.
		//@{
		uint64_t Starts = 0;
		uint64_t Refused = 0;
		//@}
		// Collects copied events and schedules playback at an exact sample deadline.
		void Sync(
			engine::ecs::Store &store,
			engine::audio::AudioMixer &mixer,
			engine::core::Vector3 listener,
			SoundCatalogue &catalogue
		);
		// Retries terminal mixer removals; returns true once all nodes were retired.
		[[nodiscard]] bool Clear(engine::audio::AudioMixer &mixer);
	};
}
