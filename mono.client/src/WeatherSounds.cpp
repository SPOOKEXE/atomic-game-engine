#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/WeatherClock.hpp>
#include <engine/weather/Runtime.hpp>
#include <engine/weather/Weather.hpp>

#include <algorithm>
#include <client/Sounds.hpp>
#include <client/WeatherSounds.hpp>
#include <cmath>
#include <limits>
#include <memory>
#include <string>

namespace client {
	namespace {
		constexpr double THUNDER_SECONDS = 2.0;
		constexpr float MAX_THUNDER_GAIN = 0.3f;
		constexpr double SCHEDULE_HORIZON_SECONDS = 0.25;

		engine::audio::SoundRef BuildThunder(engine::audio::AudioFormat format) {
			ENGINE_PROFILE("weather thunder waveform");
			auto samples = std::make_shared<engine::audio::SampleBuffer>(
				format, static_cast<size_t>(std::ceil(THUNDER_SECONDS * format.SampleRate))
			);
			float low = 0, slower = 0;
			uint32_t random = 0x54484e44u;
			for (size_t frame = 0; frame < samples->Frames(); ++frame) {
				random = engine::weather::Hash(random + 1u);
				const float noise = float(random >> 8u) * (2.0f / 16777216.0f) - 1;
				low += (noise - low) * 0.035f;
				slower += (low - slower) * 0.01f;
				const float age = float(frame) / format.SampleRate;
				const float envelope = std::min(1.0f, age * 80.0f) * std::exp(-2.8f * age);
				const float sample = std::clamp((low * 2.0f + slower * 3.0f) * envelope, -1.0f, 1.0f);
				for (uint16_t channel = 0; channel < format.Channels; ++channel)
					samples->Data()[frame * format.Channels + channel] = sample;
			}
			engine::core::Metrics::Count(
				"weather.thunder.waveform_bytes", samples->Data().size() * sizeof(float)
			);
			return samples;
		}

		bool Close(engine::audio::AudioMixer &mixer, const WeatherSoundVoice &voice) {
			auto &queue = mixer.Commands();
			if (queue.Free() < 2) return false;
			engine::audio::Command command;
			command.AtSample = mixer.Clock();
			command.Target = voice.Player;
			command.Kind = engine::audio::CommandKind::Stop;
			(void)queue.Post(command);
			command.Kind = engine::audio::CommandKind::RemoveNode;
			(void)queue.Post(command);
			return true;
		}
	}

	bool WeatherThunderDueSoon(
		engine::ecs::Store &store, engine::core::Vector3 listenerPosition, double horizonSeconds
	) {
		if (!std::isfinite(horizonSeconds) || horizonSeconds < 0) return false;
		engine::weather::PrepareWeatherRuntime(store);
		if (!store.HasResource<engine::weather::WeatherListener>())
			store.SetResource(engine::weather::WeatherListener{});
		auto &listener = *store.ResourceMutable<engine::weather::WeatherListener>();
		listener.PositionMetres = listenerPosition;
		const auto environment = engine::scene::EnvironmentOf(store);
		if (environment.HasWeatherCompute) {
			if (const auto air =
					engine::weather::QueryWeather(store, environment.CloudProvider, listenerPosition))
				listener.TemperatureCelsius = air->Air.TemperatureCelsius;
		}
		const auto *runtime = store.Resource<engine::weather::WeatherRuntimeCache>();
		if (!runtime) return false;
		bool due = false;
		for (const auto &layer : runtime->Layers) {
			const auto *controls = engine::scene::WeatherControlsOf(store, layer.Provider);
			if (controls == nullptr || !controls->Enabled) continue;
			const auto clock = engine::scene::WeatherClockOf(store, layer.Provider);
			if (const auto *events = store.Get<engine::weather::WeatherEvents>(layer.Provider)) {
				for (const auto &event : events->Events)
					(void)engine::weather::QueueThunder(
						listener.Thunder, event, listenerPosition, listener.TemperatureCelsius, clock.Tick
					);
			}
			if (clock.Paused || clock.WorldPaused || clock.TimeScaleQ16 == 0) continue;
			for (const auto &arrival : listener.Thunder.Pending) {
				const auto &event = arrival.Event;
				if (event.Layer != layer.Layer || event.Id.Epoch != clock.Epoch || event.TickRate == 0)
					continue;
				const auto remaining = engine::weather::RemainingThunderSeconds(
					arrival,
					clock.ElapsedSeconds,
					clock.ElapsedFractionQ32,
					clock.TickRate == 0 ? 0 : clock.Fraction / clock.TickRate
				);
				due |= remaining && *remaining <= horizonSeconds;
			}
		}
		return due;
	}

	void WeatherSoundStage::Sync(
		engine::ecs::Store &store,
		engine::audio::AudioMixer &mixer,
		engine::core::Vector3 listenerPosition,
		SoundCatalogue &catalogue
	) {
		ENGINE_PROFILE("weather thunder sync");
		(void)WeatherThunderDueSoon(store, listenerPosition);
		auto &listener = *store.ResourceMutable<engine::weather::WeatherListener>();
		const uint64_t sampleClock = mixer.Clock();
		// Consume begun arrivals before retiring bindings, including a sync which
		// skips the whole waveform. Otherwise its copied event could play twice.
		std::erase_if(listener.Thunder.Pending, [&](const auto &arrival) {
			return std::any_of(Voices.begin(), Voices.end(), [&](const auto &voice) {
				return voice.Event.Layer == arrival.Event.Layer && voice.Event.Id == arrival.Event.Id &&
					   sampleClock >= voice.StartSample;
			});
		});
		std::erase_if(Voices, [&](const WeatherSoundVoice &voice) {
			const auto *controls = engine::scene::WeatherControlsOf(store, voice.Provider);
			const auto clock = engine::scene::WeatherClockOf(store, voice.Provider);
			const bool retired =
				controls == nullptr || !controls->Enabled || clock.Epoch != voice.Event.Id.Epoch;
			const bool cancelFuture = sampleClock < voice.StartSample &&
									  (clock.Paused || clock.WorldPaused || clock.TimeScaleQ16 == 0);
			if (!retired && !cancelFuture && sampleClock < voice.EndSample) return false;
			if (Close(mixer, voice)) return true;
			++Refused;
			return false;
		});
		const auto *runtime = store.Resource<engine::weather::WeatherRuntimeCache>();
		if (!runtime) return;
		for (const auto &layer : runtime->Layers) {
			const auto *controls = engine::scene::WeatherControlsOf(store, layer.Provider);
			if (controls == nullptr || !controls->Enabled) continue;
			const auto clock = engine::scene::WeatherClockOf(store, layer.Provider);
			if (clock.Paused || clock.WorldPaused || clock.TimeScaleQ16 == 0) continue;
			for (const auto &arrival : listener.Thunder.Pending) {
				const auto &event = arrival.Event;
				if (event.Layer != layer.Layer || event.Id.Epoch != clock.Epoch || event.TickRate == 0)
					continue;
				const auto existing = std::find_if(Voices.begin(), Voices.end(), [&](const auto &voice) {
					return voice.Event.Layer == event.Layer && voice.Event.Id == event.Id;
				});
				if (existing != Voices.end()) continue;
				const auto remaining = engine::weather::RemainingThunderSeconds(
					arrival,
					clock.ElapsedSeconds,
					clock.ElapsedFractionQ32,
					clock.TickRate == 0 ? 0 : clock.Fraction / clock.TickRate
				);
				if (!remaining || *remaining > SCHEDULE_HORIZON_SECONDS ||
					Voices.size() >= engine::weather::MAX_WEATHER_EVENTS)
					continue;
				auto &queue = mixer.Commands();
				if (queue.Free() < 5) {
					++Refused;
					continue;
				}
				if (!Thunder || Thunder->Format() != mixer.Format()) {
					const engine::core::Name name(
						"builtin.weather.thunder." + std::to_string(mixer.Format().SampleRate) + "." +
						std::to_string(mixer.Format().Channels)
					);
					Thunder = catalogue.Find(name);
					if (!Thunder) {
						Thunder = BuildThunder(mixer.Format());
						// Retaining a catalogue reference prevents terminal mixer
						// commands from freeing a waveform on the audio callback.
						if (!catalogue.Add(name, Thunder)) {
							Thunder.reset();
							continue;
						}
					}
				}
				const auto frames =
					static_cast<uint64_t>(std::llround(*remaining * mixer.Format().SampleRate));
				if (frames > UINT64_MAX - sampleClock ||
					Thunder->Frames() > UINT64_MAX - sampleClock - frames)
					continue;
				WeatherSoundVoice voice;
				voice.Provider = layer.Provider;
				voice.Event = event;
				voice.Player = queue.Allocate();
				voice.StartSample = sampleClock + frames;
				voice.EndSample = voice.StartSample + Thunder->Frames();
				engine::audio::Command command;
				command.AtSample = sampleClock;
				command.Target = voice.Player;
				command.Kind = engine::audio::CommandKind::AddNode;
				command.Node = engine::audio::NodeKind::Player;
				(void)queue.Post(command);
				command.Kind = engine::audio::CommandKind::Connect;
				command.Second = mixer.Graph().Output();
				(void)queue.Post(command);
				command.Kind = engine::audio::CommandKind::SetSound;
				command.Sound = Thunder;
				(void)queue.Post(command);
				command.Kind = engine::audio::CommandKind::SetGain;
				command.Sound.reset();
				command.Value = MAX_THUNDER_GAIN / (1.0f + float(arrival.DelaySeconds) * 0.25f);
				(void)queue.Post(command);
				command.Kind = engine::audio::CommandKind::Play;
				command.AtSample = voice.StartSample;
				command.RetainDeadline = true;
				(void)queue.Post(command);
				Voices.push_back(std::move(voice));
				++Starts;
				engine::core::Metrics::Count("weather.thunder.starts", 1);
			}
		}
		// Keep future arrivals in ECS so suspension can cancel their queued Play
		// and resume it against the accepted clock without accepting the event twice.
		std::erase_if(listener.Thunder.Pending, [&](const auto &arrival) {
			return std::any_of(Voices.begin(), Voices.end(), [&](const auto &voice) {
				return voice.Event.Layer == arrival.Event.Layer && voice.Event.Id == arrival.Event.Id &&
					   sampleClock >= voice.StartSample;
			});
		});
	}

	bool WeatherSoundStage::Clear(engine::audio::AudioMixer &mixer) {
		std::erase_if(Voices, [&](const auto &voice) {
			if (Close(mixer, voice)) return true;
			++Refused;
			return false;
		});
		if (Voices.empty()) Thunder.reset();
		return Voices.empty();
	}
}
