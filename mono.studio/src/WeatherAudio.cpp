#include "WeatherAudio.hpp"

#include <engine/ecs/Store.hpp>

#include <algorithm>

namespace studio {
	WeatherAudio::~WeatherAudio() {
		// Join the callback before the catalogue drops its waveform references.
		Device.reset();
	}

	void WeatherAudio::Sync(engine::ecs::Store &store, engine::core::Vector3 listener) {
		const bool demanded = client::WeatherThunderDueSoon(store, listener);
		if (!Device && demanded) {
			const auto now = std::chrono::steady_clock::now();
			if (now >= RetryAfter) {
				Device = engine::audio::OpenDevice({});
				RetryAfter = now + std::chrono::seconds(1);
			}
		}
		if (!Device) return;
		std::erase_if(Stages, [&](auto &entry) {
			return entry.first != store.Identity() && entry.second.Clear(Device->Mixer());
		});
		Stages[store.Identity()].Sync(store, Device->Mixer(), listener, Catalogue);
	}

	void WeatherAudio::Silence() {
		if (!Device) return;
		std::erase_if(Stages, [&](auto &entry) { return entry.second.Clear(Device->Mixer()); });
	}
}
