#pragma once

// Copied weather time and wind anchors. The world's simulation clock stays local.
// @tier L7 · shared

#include <engine/ecs/Entity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace engine::core {
	class ByteReader;
	class ByteWriter;
}
namespace engine::ecs {
	class Store;
}

namespace engine::scene {
	// Named clock schema revision; unsupported revisions are refused before publication.
	inline constexpr uint32_t WEATHER_CLOCK_ENCODING_VERSION = 2;
	// Accepted physical elapsed time uses integer seconds and a bounded fractional second.
	struct WeatherElapsedTime {
		// Whole elapsed seconds and the Q32 fraction of the next second.
		//@{
		uint64_t Seconds = 0;
		uint32_t FractionQ32 = 0;
		//@}
	};
	// One canonical base-rate clock and displacement anchor per authored weather layer.
	struct WeatherClock {
		// Epoch identifies a timeline replacement; Sequence orders samples within it.
		//@{
		uint64_t Epoch = 1;
		uint64_t Sequence = 0;
		//@}
		// Most recent authoritative simulation tick stepped; prevents duplicate stepping after load.
		uint64_t SimulationTick = 0;
		// Integral base-rate tick plus a bounded fractional tick, never an absolute floating time.
		//@{
		uint64_t Tick = 0;
		double Fraction = 0.0;
		uint32_t TickRate = 20;
		//@}
		// Physical duration of accepted integral ticks, independent of authored TimeScale.
		//@{
		uint64_t ElapsedSeconds = 0;
		uint32_t ElapsedFractionQ32 = 0;
		// Retains sub-Q32 division residue so different tick batching gives identical elapsed time.
		uint32_t ElapsedRemainder = 0;
		//@}
		// Accepted canonical phase and cumulative modulo-2^32 Q8-cell displacement.
		//@{
		uint64_t AnchorTick = 0;
		uint64_t PhaseTick = 0;
		std::array<uint32_t, 3> DisplacementQ8{};
		std::array<uint32_t, 3> DisplacementRemainderQ12{};
		std::array<int32_t, 3> WindQ20{};
		uint32_t TimeScaleQ16 = 65536;
		uint32_t PhaseFractionQ16 = 0;
		uint32_t DefinitionRevision = 1;
		bool Paused = false;
		// World suspension freezes Tick independently of the accepted layer's Paused control.
		bool WorldPaused = false;
		//@}
	};

	// Local coalesced reliable-publication status, cleared on loading a world.
	struct WeatherClockPublication {
		// True until the current suspension anchors enter every carrying client's reliable lane.
		bool Pending = false;
	};
	// Persistent world suspension state, independent of every authored layer's pause control.
	struct WeatherWorldPause {
		// Stops layers while the owning world is suspended.
		bool Paused = false;
	};
	// One copied reliable message waiting for its provider's joining snapshot.
	struct WeatherClockArrival {
		// Authoritative provider handle and accepted copied clock.
		//@{
		ecs::Entity Provider;
		WeatherClock Clock;
		//@}
		// Local receipt evidence, retained while the join is incomplete.
		//@{
		double ReceiptSeconds = 0.0;
		double RoundTripSeconds = 0.0;
		//@}
	};
	// Bounded local receipt history survives snapshot replacement, then clears on load.
	struct WeatherClockInbox {
		// One latest arrival per provider; authored layers are bounded to thirty-two.
		std::array<WeatherClockArrival, 32> Arrivals{};
		// Number of occupied prefix entries.
		uint32_t Count = 0;
	};

	// Local receipt evidence and visual clock. Its codec clears it on save/load.
	struct WeatherClockEstimate {
		// Last accepted copied anchor and current interpolated position.
		//@{
		WeatherClock Accepted;
		uint64_t Tick = 0;
		double Fraction = 0.0;
		//@}
		// Monotonic receipt evidence, RTT low-pass estimate, and bounded slew debt in ticks.
		//@{
		double LastSeconds = 0.0;
		double SampleSeconds = 0.0;
		double RoundTripSeconds = 0.0;
		double CorrectionTicks = 0.0;
		//@}
		// Incremented when history must be discarded after a discontinuous reconstruction.
		uint64_t HistoryRevision = 0;
		// False until a valid sample is accepted.
		bool Ready = false;
	};

	// Refuses malformed ranges and anchors beginning after their sample tick.
	[[nodiscard]] bool ValidWeatherClock(const WeatherClock &clock);
	// Fixed named encoding, lossless for the entire uint64 tick range.
	void WriteWeatherClock(core::ByteWriter &writer, const WeatherClock &clock);
	// Transactional decode: refusal leaves the destination unchanged.
	[[nodiscard]] bool ReadWeatherClock(core::ByteReader &reader, WeatherClock &clock);
	// Advances only the bounded fractional part before doing checked integer addition.
	[[nodiscard]] bool AdvanceWeatherTick(uint64_t &tick, double &fraction, double ticks);
	// Advances physical elapsed time by accepted integral ticks at the current TickRate.
	[[nodiscard]] bool AdvanceWeatherElapsed(WeatherClock &clock, uint64_t ticks);
	// Resolves physical elapsed time within the accepted rate anchor, including visual extrapolation.
	[[nodiscard]] std::optional<WeatherElapsedTime>
	WeatherElapsedAtTick(const WeatherClock &clock, uint64_t tick);
	// Uses RTT/2 as a symmetric-path estimate; asymmetric latency remains uncertain.
	// Small offsets slew at at most ten percent of the base tick rate. Offsets over
	// two seconds reconstruct immediately and increment HistoryRevision.
	[[nodiscard]] bool SampleWeatherClock(
		WeatherClockEstimate &estimate, const WeatherClock &sample, double nowSeconds, double roundTripSeconds
	);
	// Extrapolation stops after two seconds without a fresh sample.
	void AdvanceWeatherClock(WeatherClockEstimate &estimate, double nowSeconds);
	// Registers the copied clock and explicitly local receipt-state codecs.
	void RegisterWeatherClockComponents();
	// Applies queued reliable anchors before advancing each layer's local visual estimate.
	void SynchronizeWeatherClocks(ecs::Store &store, double nowSeconds, double roundTripSeconds);
	// Returns the accepted visual clock, or the exact authority clock, or a safe zero clock.
	[[nodiscard]] WeatherClock WeatherClockOf(const ecs::Store &store, ecs::Entity provider);
	// Frames a copied authoritative clock for the existing reliable application-message lane.
	void WriteWeatherClockUpdate(core::ByteWriter &writer, ecs::Entity provider, const WeatherClock &clock);
	// Consumes recognized clock messages; malformed/stale arrivals do not change clocks.
	// Latest receipts remain local so older joining snapshots cannot undo an accepted anchor.
	[[nodiscard]] bool AcceptWeatherClockUpdate(
		ecs::Store &store, std::span<const std::byte> bytes, double nowSeconds, double roundTripSeconds
	);
}
