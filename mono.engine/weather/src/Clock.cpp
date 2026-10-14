#include <engine/core/Bytes.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/weather/Clock.hpp>
#include <engine/weather/Runtime.hpp>
#include <engine/weather/StatefulRuntime.hpp>

#include <algorithm>
#include <cmath>

namespace engine::weather {
	namespace {
		bool ValidEpochLayer(std::string_view layer) {
			return !layer.empty() && layer.size() <= MAX_WEATHER_LAYER_NAME_BYTES &&
				   layer.find('\0') == std::string_view::npos;
		}
		void WriteEpochHistories(core::ByteWriter &writer, const void *values, size_t count) {
			const auto *histories = static_cast<const WeatherEpochHistory *>(values);
			for (size_t index = 0; index < count; ++index)
				WriteWeatherEpochHistory(writer, histories[index]);
		}
		void ReadEpochHistories(core::ByteReader &reader, void *values, size_t count) {
			auto *histories = static_cast<WeatherEpochHistory *>(values);
			for (size_t index = 0; index < count; ++index)
				if (!ReadWeatherEpochHistory(reader, histories[index])) return;
		}
		bool NextSample(ecs::Store &store, std::string_view layer, scene::WeatherClock &clock) {
			if (clock.Sequence != UINT64_MAX) {
				++clock.Sequence;
				return true;
			}
			const auto resolved = WeatherTimeOf(clock);
			if (!resolved) return false;
			const auto epoch = ClaimWeatherEpoch(store, layer, clock.Epoch);
			if (!epoch) return false;
			clock.Epoch = *epoch;
			clock.Sequence = 1;
			clock.AnchorTick = clock.Tick;
			clock.PhaseTick = resolved->PhaseTick;
			clock.PhaseFractionQ16 = resolved->PhaseFractionQ16;
			clock.DisplacementQ8 = resolved->DisplacementQ8;
			clock.DisplacementRemainderQ12 = resolved->DisplacementRemainderQ12;
			return true;
		}
		void StoreAnchor(scene::WeatherClock &clock, const WeatherTimelineAnchor &anchor) {
			clock.AnchorTick = anchor.Tick;
			clock.PhaseTick = anchor.PhaseTick;
			clock.DisplacementQ8 = anchor.DisplacementQ8;
			clock.DisplacementRemainderQ12 = anchor.DisplacementRemainderQ12;
			clock.WindQ20 = anchor.WindQ20;
			clock.TimeScaleQ16 = anchor.TimeScaleQ16;
			clock.PhaseFractionQ16 = anchor.PhaseFractionQ16;
			clock.DefinitionRevision = anchor.Revision;
			clock.Paused = anchor.Paused;
		}
	}

	void RegisterWeatherEpochHistory() {
		ecs::Components::Register<WeatherEpochHistory>(
			"weather.EpochHistory", WriteEpochHistories, ReadEpochHistories
		);
	}

	void WriteWeatherEpochHistory(core::ByteWriter &writer, const WeatherEpochHistory &history) {
		writer.WriteString("weather.epoch-history.v1");
		writer.WriteUInt32(static_cast<uint32_t>(history.Entries.size()));
		for (const auto &record : history.Entries) {
			writer.WriteString(record.Layer);
			writer.WriteUInt64(record.Epoch);
		}
	}

	bool ReadWeatherEpochHistory(core::ByteReader &reader, WeatherEpochHistory &history) {
		if (reader.ReadString() != "weather.epoch-history.v1") {
			reader.Fail();
			return false;
		}
		const auto count = reader.ReadUInt32();
		if (reader.Failed() || count > MAX_WEATHER_EPOCH_STREAMS) {
			reader.Fail();
			return false;
		}
		WeatherEpochHistory decoded;
		decoded.Entries.reserve(count);
		for (uint32_t index = 0; index < count; ++index) {
			const auto layer = reader.ReadString();
			const auto epoch = reader.ReadUInt64();
			if (reader.Failed() || !ValidEpochLayer(layer) ||
				(!decoded.Entries.empty() && decoded.Entries.back().Layer >= layer)) {
				reader.Fail();
				return false;
			}
			decoded.Entries.push_back({std::string(layer), epoch});
		}
		history = std::move(decoded);
		return true;
	}

	bool ObserveWeatherEpoch(ecs::Store &store, std::string_view layer, uint64_t epoch) {
		if (store.AdoptOnly() || !ValidEpochLayer(layer)) return false;
		const auto *saved = store.Resource<WeatherEpochHistory>();
		if (saved) {
			const auto found = std::lower_bound(
				saved->Entries.begin(),
				saved->Entries.end(),
				layer,
				[](const auto &record, std::string_view name) { return record.Layer < name; }
			);
			if (found != saved->Entries.end() && found->Layer == layer && found->Epoch >= epoch) return true;
			if ((found == saved->Entries.end() || found->Layer != layer) &&
				saved->Entries.size() >= MAX_WEATHER_EPOCH_STREAMS)
				return false;
		}
		WeatherEpochHistory history;
		if (saved) history = *saved;
		const auto found = std::lower_bound(
			history.Entries.begin(),
			history.Entries.end(),
			layer,
			[](const auto &record, std::string_view name) { return record.Layer < name; }
		);
		if (found != history.Entries.end() && found->Layer == layer)
			found->Epoch = epoch;
		else
			history.Entries.insert(found, {std::string(layer), epoch});
		store.SetResource(history);
		return true;
	}

	std::optional<uint64_t> ClaimWeatherEpoch(ecs::Store &store, std::string_view layer, uint64_t after) {
		if (store.AdoptOnly() || !ValidEpochLayer(layer)) return {};
		uint64_t highwater = after;
		if (const auto *history = store.Resource<WeatherEpochHistory>()) {
			const auto found = std::lower_bound(
				history->Entries.begin(),
				history->Entries.end(),
				layer,
				[](const auto &record, std::string_view name) { return record.Layer < name; }
			);
			if (found != history->Entries.end() && found->Layer == layer)
				highwater = std::max(highwater, found->Epoch);
			else if (history->Entries.size() >= MAX_WEATHER_EPOCH_STREAMS)
				return {};
		}
		if (highwater == UINT64_MAX || !ObserveWeatherEpoch(store, layer, highwater + 1)) return {};
		return highwater + 1;
	}

	WeatherTimelineAnchor WeatherAnchorOf(const scene::WeatherClock &clock) {
		return WeatherTimelineAnchor{
			.Tick = clock.AnchorTick,
			.PhaseTick = clock.PhaseTick,
			.DisplacementQ8 = clock.DisplacementQ8,
			.DisplacementRemainderQ12 = clock.DisplacementRemainderQ12,
			.WindQ20 = clock.WindQ20,
			.TimeScaleQ16 = clock.TimeScaleQ16,
			.PhaseFractionQ16 = clock.PhaseFractionQ16,
			.Revision = clock.DefinitionRevision,
			.Paused = clock.Paused,
		};
	}

	std::optional<WeatherTime> WeatherTimeOf(const scene::WeatherClock &clock) {
		if (!scene::ValidWeatherClock(clock)) return {};
		return ResolveTime(WeatherAnchorOf(clock), clock.Tick);
	}

	std::optional<WeatherElapsedTime> ElapsedAtTick(const scene::WeatherClock &clock, uint64_t tick) {
		if (tick > clock.Tick) return {};
		return scene::WeatherElapsedAtTick(clock, tick);
	}

	void UpdateWeatherClocks(ecs::Store &store) {
		ENGINE_PROFILE("weather authority clocks");
		if (store.AdoptOnly()) return;
		const auto time = store.Time();
		const auto *worldPause = store.Resource<scene::WeatherWorldPause>();
		const bool worldPaused = worldPause && worldPause->Paused;
		if (!std::isfinite(time.Delta) || time.Delta < 0) return;
		PrepareWeatherRuntime(store, true);
		const auto *cache = store.Resource<WeatherRuntimeCache>();
		if (!cache) return;
		for (const auto &layer : cache->Layers) {
			if (!layer.Compiled || !store.Alive(layer.Provider)) continue;
			const auto *stored = store.Get<scene::WeatherClock>(layer.Provider);
			if (stored && !scene::ValidWeatherClock(*stored)) continue;
			if (stored && !ObserveWeatherEpoch(store, layer.Layer, stored->Epoch)) continue;
			if (stored && stored->SimulationTick >= time.Tick) continue;
			auto definition = layer.Compiled->Definition;
			const auto *controls = scene::WeatherControlsOf(store, layer.Provider);
			definition.Paused = definition.Paused || (controls && !controls->Enabled);
			scene::WeatherClock clock;
			if (stored)
				clock = *stored;
			else {
				const auto epoch = ClaimWeatherEpoch(store, layer.Layer);
				if (!epoch) continue;
				clock.Epoch = *epoch;
				clock.TickRate = definition.TickRate;
				StoreAnchor(clock, InitialAnchor(definition));
			}
			const bool changed = clock.DefinitionRevision != definition.Revision ||
								 clock.WindQ20 != definition.WindQ20 ||
								 clock.TimeScaleQ16 != definition.TimeScaleQ16 ||
								 clock.Paused != definition.Paused || clock.TickRate != definition.TickRate;
			if (changed) {
				const auto changed = Reanchor(WeatherAnchorOf(clock), definition, clock.Tick);
				if (!changed) continue;
				StoreAnchor(clock, *changed);
				if (clock.TickRate != definition.TickRate) clock.ElapsedRemainder = 0;
				// A new rate starts at the same bounded fraction of its current base tick.
				clock.TickRate = definition.TickRate;
			}
			const bool frozen = clock.Paused || worldPaused || clock.TimeScaleQ16 == 0;
			if (stored && !changed && (frozen || time.Delta == 0)) continue;
			const auto beforeTick = clock.Tick;
			if (!frozen)
				(void)scene::AdvanceWeatherTick(
					clock.Tick, clock.Fraction, static_cast<double>(time.Delta) * clock.TickRate
				);
			(void)scene::AdvanceWeatherElapsed(clock, clock.Tick - beforeTick);
			clock.SimulationTick = time.Tick;
			clock.WorldPaused = worldPaused;
			if (!NextSample(store, layer.Layer, clock)) continue;
			store.Set(layer.Provider, clock);
		}
	}

	bool SetWeatherWorldPaused(ecs::Store &store, bool paused) {
		if (store.AdoptOnly()) return false;
		const auto *before = store.Resource<scene::WeatherWorldPause>();
		if ((before && before->Paused == paused) || (!before && !paused)) return false;
		store.SetResource(scene::WeatherWorldPause{paused});
		PrepareWeatherRuntime(store);
		const auto *cache = store.Resource<WeatherRuntimeCache>();
		if (!cache) return false;
		bool changed = false;
		for (const auto &layer : cache->Layers) {
			if (!store.Alive(layer.Provider)) continue;
			scene::WeatherClock clock;
			if (const auto *stored = store.Get<scene::WeatherClock>(layer.Provider)) {
				if (!scene::ValidWeatherClock(*stored)) continue;
				if (!ObserveWeatherEpoch(store, layer.Layer, stored->Epoch)) continue;
				clock = *stored;
			} else {
				if (!layer.Compiled) continue;
				const auto epoch = ClaimWeatherEpoch(store, layer.Layer);
				if (!epoch) continue;
				clock.Epoch = *epoch;
				auto definition = layer.Compiled->Definition;
				const auto *controls = scene::WeatherControlsOf(store, layer.Provider);
				definition.Paused = definition.Paused || (controls && !controls->Enabled);
				StoreAnchor(clock, InitialAnchor(definition));
				clock.TickRate = definition.TickRate;
			}
			if (!NextSample(store, layer.Layer, clock)) continue;
			clock.WorldPaused = paused;
			store.Set(layer.Provider, clock);
			changed = true;
		}
		if (changed) store.SetResource(scene::WeatherClockPublication{true});
		return changed;
	}

	void RegisterWeatherClockSystem(ecs::Scheduler &scheduler) {
		scheduler.Add("weather.clock", ecs::Phase::PreSimulation, UpdateWeatherClocks);
		RegisterStatefulWeatherSystem(scheduler);
		RegisterWeatherRuntimeSystem(scheduler);
	}
}
