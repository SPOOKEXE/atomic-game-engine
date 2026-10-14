#pragma once

// Authority clock stepping and direct reconstruction adapters. No device state.
// @tier L8 · shared

#include <engine/scene/WeatherClock.hpp>
#include <engine/weather/Weather.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::core {
	class ByteReader;
	class ByteWriter;
}

namespace engine::ecs {
	class Scheduler;
	class Store;
}

namespace engine::weather {
	// Maximum distinct named streams retained across provider retirement; never evicted.
	inline constexpr uint32_t MAX_WEATHER_EPOCH_STREAMS = 256;
	// Tombstone identity is a canonical layer name, independent of entity allocation order.
	struct WeatherEpochRecord {
		// Accepted canonical layer path, serialized as text.
		std::string Layer;
		// Zero reserves a name before the first authority clock is created.
		uint64_t Epoch = 0;
		// Compares the portable name and high-water mark.
		bool operator==(const WeatherEpochRecord &) const = default;
	};
	// Authority-owned saved history. Records stay sorted by canonical layer name; no eviction.
	struct WeatherEpochHistory {
		// Bounded lexicographically sorted records, including retired streams and reservations.
		std::vector<WeatherEpochRecord> Entries;
		// Compares persisted semantic records without object padding.
		bool operator==(const WeatherEpochHistory &) const = default;
	};
	// Registers the authority-owned resource's complete save codec.
	void RegisterWeatherEpochHistory();
	// Writes the named version and each bounded text identity and integer epoch.
	void WriteWeatherEpochHistory(core::ByteWriter &writer, const WeatherEpochHistory &history);
	// Refusal preserves the destination and marks the reader failed.
	[[nodiscard]] bool ReadWeatherEpochHistory(core::ByteReader &reader, WeatherEpochHistory &history);
	// Reserves a name or imports a saved/current epoch. Refuses replica writes and exhausted name budget.
	[[nodiscard]] bool ObserveWeatherEpoch(ecs::Store &store, std::string_view layer, uint64_t epoch);
	// Claims max(saved high-water, after)+1; refusal leaves history unchanged, including at UINT64_MAX.
	[[nodiscard]] std::optional<uint64_t>
	ClaimWeatherEpoch(ecs::Store &store, std::string_view layer, uint64_t after = 0);
	using WeatherElapsedTime = scene::WeatherElapsedTime;
	// Resolves a past accepted integral tick without converting large absolute ages to floating point.
	[[nodiscard]] std::optional<WeatherElapsedTime>
	ElapsedAtTick(const scene::WeatherClock &clock, uint64_t tick);
	// Converts copied canonical phase and wind values without lossy tick conversion.
	[[nodiscard]] WeatherTimelineAnchor WeatherAnchorOf(const scene::WeatherClock &clock);
	// Resolves the accepted integral tick; Fraction remains a visual interpolation input.
	[[nodiscard]] std::optional<WeatherTime> WeatherTimeOf(const scene::WeatherClock &clock);
	// Advances authored layers from fixed simulation time, preserving anchors before edits.
	// Replica stores are ignored. Repeated calls within one world tick do no work.
	void UpdateWeatherClocks(ecs::Store &store);
	// Registers the headless fixed-tick pass before weather consumers.
	void RegisterWeatherClockSystem(ecs::Scheduler &scheduler);
	// Suspends or resumes the base tick without changing historical authored anchors.
	// Returns whether at least one canonical layer changed; requests reliable final publication.
	[[nodiscard]] bool SetWeatherWorldPaused(ecs::Store &store, bool paused);
}
