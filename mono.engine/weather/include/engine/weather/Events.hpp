#pragma once

// Copied, named storm events and listener-local thunder scheduling. No GPU or network state.
// @tier L8 · shared

#include <engine/core/types/Vector3.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::core {
	class ByteReader;
	class ByteWriter;
}

namespace engine::weather {
	// Explicit bounds on retained events and portable layer identities.
	inline constexpr uint32_t MAX_WEATHER_EVENTS = 32;
	// Maximum UTF-8 bytes in one copied layer identity.
	inline constexpr uint32_t MAX_WEATHER_LAYER_NAME_BYTES = 256;
	// Gameplay event kind, serialized by stable name.
	enum class StormEventKind : uint8_t {
		// A charge discharge with a reproducible branch seed.
		Lightning,
		// One grown hail impact with bounded damage and radius.
		Hail
	};
	// Identity remains unique over save/load, reseeding and timeline replacement.
	struct WeatherEventId {
		// Accepted timeline generation and sequence assigned once by authority.
		//@{
		uint64_t Epoch = 1;
		uint64_t Sequence = 0;
		//@}
		// Weather seed distinguishes recreated layers with a different source.
		uint32_t Seed = 1;
		// Compares portable identity values.
		bool operator==(const WeatherEventId &) const = default;
	};
	// One headless authoritative event. Damage adapters consume these copied values once.
	struct StormEvent {
		// Stable authored layer path; never a process-local Name id.
		std::string Layer;
		// Authority-assigned event identity.
		WeatherEventId Id;
		// Kind chooses lightning discharge or a bounded hail impact.
		StormEventKind Kind = StormEventKind::Lightning;
		// Integral accepted weather tick and its declared base-rate ticks/second.
		//@{
		uint64_t Tick = 0;
		uint32_t TickRate = 20;
		//@}
		// Acoustic elapsed at the accepted integral event tick. Integer-first subtraction preserves
		// subsecond delays at large world ages and across base-rate edits; TimeScale does not scale sound.
		//@{
		uint64_t ElapsedSeconds = 0;
		uint32_t ElapsedFractionQ32 = 0;
		//@}
		// Copied world endpoints in metres; hail uses Start as its cloud origin.
		//@{
		core::Vector3 StartMetres;
		core::Vector3 EndMetres;
		//@}
		// Branch detail is reproducible without any additional random state.
		uint32_t BranchSeed = 0;
		// Explicit bounded gameplay response in health units and world metres.
		//@{
		float Damage = 0.0f;
		float RadiusMetres = 0.0f;
		//@}
		// Compares the copied payload including identity.
		bool operator==(const StormEvent &) const = default;
	};
	// Bounded recent events replicated with the authored provider.
	struct WeatherEvents {
		// Chronological retained suffix; full queues discard oldest records explicitly.
		std::vector<StormEvent> Events;
		// Number of discarded historical records, distinct from generated events.
		uint64_t Dropped = 0;
	};
	// Fixed visual reconstruction budget for one copied lightning event.
	inline constexpr size_t MAX_LIGHTNING_SEGMENTS = 28;
	// Copied line geometry reconstructed entirely from event endpoints and branch seed.
	struct LightningSegment {
		// World-space segment endpoints in metres.
		//@{
		core::Vector3 StartMetres;
		core::Vector3 EndMetres;
		//@}
		// Zero is the main channel; one through four identify cosmetic branches.
		uint32_t Branch = 0;
		// Compares the reconstructed geometry without object padding.
		bool operator==(const LightningSegment &) const = default;
	};
	// High-water mark for one layer's exactly-once consumer.
	struct WeatherEventCursor {
		// Stable source identity and accepted epoch/sequence/tick.
		//@{
		std::string Layer;
		WeatherEventId Last;
		uint64_t Tick = 0;
		//@}
		// False until the first payload was accepted.
		bool Ready = false;
	};
	// One listener-local sound arrival; DelaySeconds uses distance at receipt.
	struct ThunderArrival {
		// Original copied lightning event.
		StormEvent Event;
		// Constant-rate due marker, saturated at the tick limit. Elapsed-time consumers support rate edits.
		uint64_t DueTick = 0;
		// Distance/sound-speed delay in seconds.
		double DelaySeconds = 0.0;
	};
	// Bounded sound queue. Each listener owns its own queue and receipt cursors.
	struct ThunderQueue {
		// Bounded arrivals in stable due-marker order; elapsed-time consumers evaluate every arrival.
		std::vector<ThunderArrival> Pending;
		// Bounded per-layer duplicate/out-of-order receipt guards.
		std::vector<WeatherEventCursor> Cursors;
	};
	// Camera-independent canonical storm column shared by procedural and stateful schedulers.
	[[nodiscard]] core::Vector3
	StormColumn(uint32_t seed, uint32_t layerKey, uint64_t tick, uint64_t epoch, core::Vector3 extentMetres);
	// Named event kind used in every durable payload.
	[[nodiscard]] const char *StormEventKindName(StormEventKind kind);
	// Refuses unknown kinds, oversized identities, invalid units and nonfinite coordinates.
	[[nodiscard]] bool ValidStormEvent(const StormEvent &event);
	// Versioned, field-by-field portable payload.
	void WriteStormEvent(core::ByteWriter &writer, const StormEvent &event);
	// Transactional decoding preserves the destination on failure.
	[[nodiscard]] bool ReadStormEvent(core::ByteReader &reader, StormEvent &event);
	// Appends valid events while bounding retained history.
	[[nodiscard]] bool AppendWeatherEvent(WeatherEvents &events, const StormEvent &event);
	// Rejects duplicates, older epochs, older sequences and backwards ticks in one epoch.
	[[nodiscard]] bool AcceptWeatherEvent(WeatherEventCursor &cursor, const StormEvent &event);
	// Reconstructs sixteen main segments and four three-segment branches, independently of any view.
	// Refusal returns zero before writing for malformed/nonlightning events or insufficient capacity.
	[[nodiscard]] size_t BuildLightningSegments(const StormEvent &event, std::span<LightningSegment> output);
	// Dry ideal-air c = sqrt(1.4 * 287.05 * Kelvin), metres/second; no humidity correction.
	[[nodiscard]] std::optional<double> SoundSpeedMetresPerSecond(float temperatureCelsius);
	// Schedules once by listener distance; the legacy tick due marker saturates at the tick limit.
	[[nodiscard]] bool QueueThunder(
		ThunderQueue &queue,
		const StormEvent &event,
		core::Vector3 listenerMetres,
		float temperatureCelsius,
		uint64_t listenerTick
	);
	// Relative age in accepted base-clock seconds. Integer-first subtraction preserves short lifetimes
	// at huge world ages. FractionalSeconds is the visual base-tick fraction in seconds.
	// Future snapshots and malformed inputs return no age.
	[[nodiscard]] std::optional<double> WeatherEventAgeSeconds(
		const StormEvent &event,
		uint64_t elapsedSeconds,
		uint32_t elapsedFractionQ32,
		double fractionalSeconds = 0.0
	);
	// Remaining acoustic delay irrespective of subsequent base-rate edits.
	// An earlier clock snapshot retains the full distance-based delay.
	[[nodiscard]] std::optional<double> RemainingThunderSeconds(
		const ThunderArrival &arrival,
		uint64_t elapsedSeconds,
		uint32_t elapsedFractionQ32,
		double fractionalSeconds = 0.0
	);
	// Copies due sounds when the layer's base rate has remained unchanged since emission.
	// Rate-edit consumers use RemainingThunderSeconds. Empty layer assumes one shared clock.
	// Caller capacity bounds removal; arrivals for other layer clocks remain queued.
	[[nodiscard]] size_t TakeDueThunder(
		ThunderQueue &queue,
		uint64_t listenerTick,
		std::span<ThunderArrival> output,
		std::string_view layer = {}
	);
}
