#pragma once

// Studio owns one output device and listens to its focused Play viewport.
// Arrival state stays in the entered world; only mixer bindings live here.

#include <engine/audio/Device.hpp>

#include <chrono>
#include <client/Sounds.hpp>
#include <client/WeatherSounds.hpp>
#include <memory>
#include <unordered_map>

namespace studio {
	struct WeatherAudio {
		std::unique_ptr<engine::audio::Device> Device;
		client::SoundCatalogue Catalogue;
		std::unordered_map<uint64_t, client::WeatherSoundStage> Stages;
		std::chrono::steady_clock::time_point RetryAfter;

		~WeatherAudio();
		void Sync(engine::ecs::Store &store, engine::core::Vector3 listener);
		void Silence();
	};
}
