#include <engine/core/Bytes.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/WeatherClock.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <vector>

namespace engine::scene {
	namespace {
		constexpr double MAXIMUM_EXTRAPOLATION_SECONDS = 2.0;
		constexpr double MAXIMUM_ROUND_TRIP_SECONDS = 10.0;
		constexpr double SLEW_FRACTION = 0.1;

		double
		TickDifference(uint64_t target, double targetFraction, uint64_t current, double currentFraction) {
			const double whole = target >= current ? static_cast<double>(target - current)
												   : -static_cast<double>(current - target);
			return whole + targetFraction - currentFraction;
		}
		void WriteClocks(core::ByteWriter &writer, const void *values, size_t count) {
			const auto *clocks = static_cast<const WeatherClock *>(values);
			for (size_t index = 0; index < count; ++index)
				WriteWeatherClock(writer, clocks[index]);
		}
		void ReadClocks(core::ByteReader &reader, void *values, size_t count) {
			auto *clocks = static_cast<WeatherClock *>(values);
			for (size_t index = 0; index < count; ++index)
				if (!ReadWeatherClock(reader, clocks[index])) return;
		}
		void WriteEstimates(core::ByteWriter &, const void *, size_t) {}
		void ReadEstimates(core::ByteReader &, void *values, size_t count) {
			auto *estimates = static_cast<WeatherClockEstimate *>(values);
			for (size_t index = 0; index < count; ++index)
				estimates[index] = {};
		}
		void WritePublications(core::ByteWriter &, const void *, size_t) {}
		void ReadPublications(core::ByteReader &, void *values, size_t count) {
			auto *publications = static_cast<WeatherClockPublication *>(values);
			for (size_t index = 0; index < count; ++index)
				publications[index] = {};
		}
		void WriteInboxes(core::ByteWriter &, const void *, size_t) {}
		void ReadInboxes(core::ByteReader &, void *values, size_t count) {
			auto *inboxes = static_cast<WeatherClockInbox *>(values);
			for (size_t index = 0; index < count; ++index)
				inboxes[index] = {};
		}
		bool
		RetainArrival(WeatherClockInbox &inbox, const WeatherClockArrival &arrival, const ecs::Store &store) {
			for (uint32_t index = 0; index < inbox.Count; ++index) {
				auto &held = inbox.Arrivals[index];
				if (held.Provider == arrival.Provider) {
					if (arrival.Clock.Epoch < held.Clock.Epoch ||
						(arrival.Clock.Epoch == held.Clock.Epoch &&
						 arrival.Clock.Sequence <= held.Clock.Sequence))
						return false;
					held = arrival;
					return true;
				}
			}
			if (inbox.Count < inbox.Arrivals.size()) {
				inbox.Arrivals[inbox.Count++] = arrival;
				return true;
			}
			// Retired providers must not exhaust the bounded receipt history after layer churn.
			uint32_t retired = inbox.Count;
			for (uint32_t index = 0; index < inbox.Count; ++index) {
				if (store.Alive(inbox.Arrivals[index].Provider)) continue;
				if (retired == inbox.Count ||
					inbox.Arrivals[index].ReceiptSeconds < inbox.Arrivals[retired].ReceiptSeconds)
					retired = index;
			}
			if (retired == inbox.Count) return false;
			inbox.Arrivals[retired] = arrival;
			return true;
		}
		bool ApplyArrival(ecs::Store &store, const WeatherClockArrival &arrival, double nowSeconds) {
			const auto *canonical = store.Get<WeatherClock>(arrival.Provider);
			if (!canonical) return false;
			const auto &sample = arrival.Clock;
			if (sample.Epoch < canonical->Epoch ||
				(sample.Epoch == canonical->Epoch && sample.Sequence <= canonical->Sequence))
				return true;
			WeatherClockEstimate estimate;
			if (const auto *old = store.Get<WeatherClockEstimate>(arrival.Provider)) estimate = *old;
			if (estimate.Ready && estimate.LastSeconds > arrival.ReceiptSeconds) {
				const auto history = estimate.HistoryRevision;
				estimate = {};
				estimate.HistoryRevision = history;
			}
			if (!SampleWeatherClock(estimate, sample, arrival.ReceiptSeconds, arrival.RoundTripSeconds))
				return true;
			AdvanceWeatherClock(estimate, nowSeconds);
			store.Set(arrival.Provider, sample);
			store.Set(arrival.Provider, estimate);
			return true;
		}
		void WriteWorldPauses(core::ByteWriter &writer, const void *values, size_t count) {
			const auto *pauses = static_cast<const WeatherWorldPause *>(values);
			for (size_t index = 0; index < count; ++index)
				writer.WriteUInt8(pauses[index].Paused ? 1 : 0);
		}
		void ReadWorldPauses(core::ByteReader &reader, void *values, size_t count) {
			auto *pauses = static_cast<WeatherWorldPause *>(values);
			for (size_t index = 0; index < count; ++index) {
				const auto paused = reader.ReadUInt8();
				if (paused > 1 || reader.Failed()) {
					reader.Fail();
					return;
				}
				pauses[index].Paused = paused != 0;
			}
		}
	}

	bool ValidWeatherClock(const WeatherClock &clock) {
		const bool residualsValid = std::all_of(
			clock.DisplacementRemainderQ12.begin(),
			clock.DisplacementRemainderQ12.end(),
			[](uint32_t residual) { return residual < 4096; }
		);
		const bool windsValid = std::all_of(clock.WindQ20.begin(), clock.WindQ20.end(), [](int32_t wind) {
			return wind >= -1073741824 && wind <= 1073741824;
		});
		return residualsValid && windsValid && clock.Epoch != 0 && clock.TickRate > 0 &&
			   clock.TickRate <= 1000 && clock.ElapsedRemainder < clock.TickRate &&
			   std::isfinite(clock.Fraction) && clock.Fraction >= 0.0 && clock.Fraction < 1.0 &&
			   clock.AnchorTick <= clock.Tick && clock.TimeScaleQ16 <= 16u * 65536u &&
			   clock.PhaseFractionQ16 < 65536 && clock.DefinitionRevision != 0;
	}

	void WriteWeatherClock(core::ByteWriter &writer, const WeatherClock &clock) {
		writer.WriteUInt32(WEATHER_CLOCK_ENCODING_VERSION);
		writer.WriteUInt64(clock.Epoch);
		writer.WriteUInt64(clock.Sequence);
		writer.WriteUInt64(clock.SimulationTick);
		writer.WriteUInt64(clock.Tick);
		writer.WriteDouble(clock.Fraction);
		writer.WriteUInt32(clock.TickRate);
		writer.WriteUInt64(clock.ElapsedSeconds);
		writer.WriteUInt32(clock.ElapsedFractionQ32);
		writer.WriteUInt32(clock.ElapsedRemainder);
		writer.WriteUInt64(clock.AnchorTick);
		writer.WriteUInt64(clock.PhaseTick);
		for (const auto displacement : clock.DisplacementQ8)
			writer.WriteUInt32(displacement);
		for (const auto residual : clock.DisplacementRemainderQ12)
			writer.WriteUInt32(residual);
		for (const auto wind : clock.WindQ20)
			writer.WriteUInt32(static_cast<uint32_t>(wind));
		writer.WriteUInt32(clock.TimeScaleQ16);
		writer.WriteUInt32(clock.PhaseFractionQ16);
		writer.WriteUInt32(clock.DefinitionRevision);
		writer.WriteUInt8(clock.Paused ? 1 : 0);
		writer.WriteUInt8(clock.WorldPaused ? 1 : 0);
	}

	bool ReadWeatherClock(core::ByteReader &reader, WeatherClock &clock) {
		const auto version = reader.ReadUInt32();
		WeatherClock decoded;
		decoded.Epoch = reader.ReadUInt64();
		decoded.Sequence = reader.ReadUInt64();
		decoded.SimulationTick = reader.ReadUInt64();
		decoded.Tick = reader.ReadUInt64();
		decoded.Fraction = reader.ReadDouble();
		decoded.TickRate = reader.ReadUInt32();
		decoded.ElapsedSeconds = reader.ReadUInt64();
		decoded.ElapsedFractionQ32 = reader.ReadUInt32();
		decoded.ElapsedRemainder = reader.ReadUInt32();
		decoded.AnchorTick = reader.ReadUInt64();
		decoded.PhaseTick = reader.ReadUInt64();
		for (auto &displacement : decoded.DisplacementQ8)
			displacement = reader.ReadUInt32();
		for (auto &residual : decoded.DisplacementRemainderQ12)
			residual = reader.ReadUInt32();
		for (auto &wind : decoded.WindQ20)
			wind = std::bit_cast<int32_t>(reader.ReadUInt32());
		decoded.TimeScaleQ16 = reader.ReadUInt32();
		decoded.PhaseFractionQ16 = reader.ReadUInt32();
		decoded.DefinitionRevision = reader.ReadUInt32();
		const auto paused = reader.ReadUInt8();
		decoded.Paused = paused != 0;
		const auto worldPaused = reader.ReadUInt8();
		decoded.WorldPaused = worldPaused != 0;
		if (reader.Failed() || version != WEATHER_CLOCK_ENCODING_VERSION || paused > 1 || worldPaused > 1 ||
			!ValidWeatherClock(decoded)) {
			reader.Fail();
			return false;
		}
		clock = decoded;
		return true;
	}

	bool AdvanceWeatherTick(uint64_t &tick, double &fraction, double ticks) {
		if (!std::isfinite(ticks) || ticks < 0 || !std::isfinite(fraction) || fraction < 0 || fraction >= 1)
			return false;
		const double total = fraction + ticks;
		const double whole = std::floor(total);
		const auto remaining = std::numeric_limits<uint64_t>::max() - tick;
		// 2^64 is exactly representable; UINT64_MAX is not. Check before converting.
		if (whole >= 18446744073709551616.0) {
			tick = std::numeric_limits<uint64_t>::max();
			fraction = 0.0;
			return true;
		}
		const auto integral = static_cast<uint64_t>(whole);
		if (integral > remaining) {
			tick = std::numeric_limits<uint64_t>::max();
			fraction = 0.0;
			return true;
		}
		tick += integral;
		fraction = tick == std::numeric_limits<uint64_t>::max() ? 0.0 : total - whole;
		return true;
	}

	bool AdvanceWeatherElapsed(WeatherClock &clock, uint64_t ticks) {
		if (clock.TickRate == 0 || clock.TickRate > 1000 || clock.ElapsedRemainder >= clock.TickRate)
			return false;
		const uint64_t numerator = ((ticks % clock.TickRate) << 32) + clock.ElapsedRemainder;
		const uint64_t fraction = clock.ElapsedFractionQ32 + numerator / clock.TickRate;
		const uint64_t seconds = ticks / clock.TickRate;
		const uint64_t carry = fraction >> 32;
		if (seconds > UINT64_MAX - clock.ElapsedSeconds ||
			carry > UINT64_MAX - clock.ElapsedSeconds - seconds) {
			clock.ElapsedSeconds = UINT64_MAX;
			clock.ElapsedFractionQ32 = UINT32_MAX;
			clock.ElapsedRemainder = clock.TickRate - 1;
			return true;
		}
		clock.ElapsedSeconds += seconds + carry;
		clock.ElapsedFractionQ32 = static_cast<uint32_t>(fraction);
		clock.ElapsedRemainder = static_cast<uint32_t>(numerator % clock.TickRate);
		return true;
	}

	std::optional<WeatherElapsedTime> WeatherElapsedAtTick(const WeatherClock &clock, uint64_t tick) {
		if (!ValidWeatherClock(clock) || tick < clock.AnchorTick) return {};
		if (tick >= clock.Tick) {
			auto extrapolated = clock;
			(void)AdvanceWeatherElapsed(extrapolated, tick - clock.Tick);
			return WeatherElapsedTime{extrapolated.ElapsedSeconds, extrapolated.ElapsedFractionQ32};
		}
		const uint64_t ticks = clock.Tick - tick;
		const uint64_t numerator = (ticks % clock.TickRate) << 32;
		const uint64_t fraction =
			numerator / clock.TickRate + (clock.ElapsedRemainder < numerator % clock.TickRate ? 1u : 0u);
		const uint64_t borrow = clock.ElapsedFractionQ32 < fraction ? 1u : 0u;
		const uint64_t seconds = ticks / clock.TickRate;
		if (seconds > clock.ElapsedSeconds || borrow > clock.ElapsedSeconds - seconds) return {};
		return WeatherElapsedTime{
			clock.ElapsedSeconds - seconds - borrow,
			static_cast<uint32_t>(static_cast<uint64_t>(clock.ElapsedFractionQ32) + (borrow << 32) - fraction)
		};
	}

	void AdvanceWeatherClock(WeatherClockEstimate &estimate, double nowSeconds) {
		if (!estimate.Ready || !std::isfinite(nowSeconds) || nowSeconds < estimate.LastSeconds) return;
		const double until = std::min(nowSeconds, estimate.SampleSeconds + MAXIMUM_EXTRAPOLATION_SECONDS);
		const double elapsed = std::max(0.0, until - estimate.LastSeconds);
		estimate.LastSeconds = nowSeconds;
		const bool frozen =
			estimate.Accepted.Paused || estimate.Accepted.WorldPaused || estimate.Accepted.TimeScaleQ16 == 0;
		const double baseTicks = frozen ? 0.0 : elapsed * estimate.Accepted.TickRate;
		const double correction =
			std::clamp(estimate.CorrectionTicks, -baseTicks * SLEW_FRACTION, baseTicks * SLEW_FRACTION);
		(void)AdvanceWeatherTick(estimate.Tick, estimate.Fraction, baseTicks + correction);
		estimate.CorrectionTicks -= correction;
	}

	bool SampleWeatherClock(
		WeatherClockEstimate &estimate, const WeatherClock &sample, double nowSeconds, double roundTripSeconds
	) {
		if (!ValidWeatherClock(sample) || !std::isfinite(nowSeconds) || !std::isfinite(roundTripSeconds) ||
			roundTripSeconds < 0 || roundTripSeconds > MAXIMUM_ROUND_TRIP_SECONDS ||
			(estimate.Ready && nowSeconds < estimate.LastSeconds))
			return false;
		if (estimate.Ready &&
			(sample.Epoch < estimate.Accepted.Epoch ||
			 (sample.Epoch == estimate.Accepted.Epoch && sample.Sequence <= estimate.Accepted.Sequence)))
			return false;
		AdvanceWeatherClock(estimate, nowSeconds);
		const bool replacement = !estimate.Ready || sample.Epoch != estimate.Accepted.Epoch ||
								 sample.DefinitionRevision != estimate.Accepted.DefinitionRevision ||
								 sample.Paused != estimate.Accepted.Paused ||
								 sample.WorldPaused != estimate.Accepted.WorldPaused ||
								 sample.TickRate != estimate.Accepted.TickRate ||
								 sample.TimeScaleQ16 != estimate.Accepted.TimeScaleQ16 ||
								 sample.WindQ20 != estimate.Accepted.WindQ20;
		estimate.RoundTripSeconds =
			estimate.Ready && sample.Epoch == estimate.Accepted.Epoch
				? estimate.RoundTripSeconds + (roundTripSeconds - estimate.RoundTripSeconds) * 0.125
				: roundTripSeconds;
		uint64_t target = sample.Tick;
		double targetFraction = sample.Fraction;
		if (!sample.Paused && !sample.WorldPaused && sample.TimeScaleQ16 != 0)
			(void)AdvanceWeatherTick(
				target, targetFraction, std::min(estimate.RoundTripSeconds * 0.5, 0.5) * sample.TickRate
			);
		const double offset = TickDifference(target, targetFraction, estimate.Tick, estimate.Fraction);
		if (replacement || std::abs(offset) > MAXIMUM_EXTRAPOLATION_SECONDS * sample.TickRate) {
			estimate.Tick = target;
			estimate.Fraction = targetFraction;
			estimate.CorrectionTicks = 0;
			if (estimate.HistoryRevision != UINT64_MAX) ++estimate.HistoryRevision;
		} else {
			estimate.CorrectionTicks = offset;
		}
		estimate.Accepted = sample;
		estimate.SampleSeconds = nowSeconds;
		estimate.LastSeconds = nowSeconds;
		estimate.Ready = true;
		return true;
	}

	void RegisterWeatherClockComponents() {
		ecs::Components::Register<WeatherClock>("scene.WeatherClock", WriteClocks, ReadClocks);
		ecs::Components::Register<WeatherClockEstimate>(
			"scene.WeatherClockEstimate", WriteEstimates, ReadEstimates
		);
		ecs::Components::Register<WeatherClockPublication>(
			"scene.WeatherClockPublication", WritePublications, ReadPublications
		);
		ecs::Components::Register<WeatherWorldPause>(
			"scene.WeatherWorldPause", WriteWorldPauses, ReadWorldPauses
		);
		ecs::Components::Register<WeatherClockInbox>("scene.WeatherClockInbox", WriteInboxes, ReadInboxes);
	}

	void SynchronizeWeatherClocks(ecs::Store &store, double nowSeconds, double roundTripSeconds) {
		ENGINE_PROFILE("weather clock synchronization");
		WeatherClockInbox receipts;
		if (const auto *inbox = store.Resource<WeatherClockInbox>()) {
			receipts = *inbox;
			for (uint32_t index = 0; index < inbox->Count; ++index) {
				const auto arrival = inbox->Arrivals[index];
				(void)ApplyArrival(store, arrival, nowSeconds);
			}
		}
		// Adding a component moves its archetype, so collect missing providers before writing.
		std::vector<ecs::Entity> missing;
		store.Each<const WeatherClock>([&](ecs::Entity provider, const WeatherClock &clock) {
			(void)clock;
			if (!store.Has<WeatherClockEstimate>(provider)) missing.push_back(provider);
		});
		for (const auto provider : missing)
			store.Set(provider, WeatherClockEstimate{});
		bool changed = false;
		store.Each<const WeatherClock, WeatherClockEstimate>(
			[&](ecs::Entity provider, const WeatherClock &sample, WeatherClockEstimate &estimate) {
				(void)SampleWeatherClock(estimate, sample, nowSeconds, roundTripSeconds);
				AdvanceWeatherClock(estimate, nowSeconds);
				if (estimate.Ready)
					changed = RetainArrival(
								  receipts, {provider, estimate.Accepted, nowSeconds, roundTripSeconds}, store
							  ) ||
							  changed;
			}
		);
		if (changed) store.SetResource(receipts);
	}

	WeatherClock WeatherClockOf(const ecs::Store &store, ecs::Entity provider) {
		if (const auto *estimate = store.Get<WeatherClockEstimate>(provider); estimate && estimate->Ready) {
			WeatherClock clock = estimate->Accepted;
			clock.Tick = std::max(estimate->Tick, clock.AnchorTick);
			clock.Fraction = estimate->Fraction;
			if (clock.Tick >= estimate->Accepted.Tick) {
				(void)AdvanceWeatherElapsed(clock, clock.Tick - estimate->Accepted.Tick);
			} else if (const auto elapsed = WeatherElapsedAtTick(estimate->Accepted, clock.Tick)) {
				clock.ElapsedSeconds = elapsed->Seconds;
				clock.ElapsedFractionQ32 = elapsed->FractionQ32;
				const uint64_t residue = ((estimate->Accepted.Tick - clock.Tick) % clock.TickRate) << 32;
				clock.ElapsedRemainder = static_cast<uint32_t>(
					(clock.ElapsedRemainder + clock.TickRate - residue % clock.TickRate) % clock.TickRate
				);
			}
			return clock;
		}
		if (const auto *clock = store.Get<WeatherClock>(provider)) return *clock;
		return {};
	}

	void WriteWeatherClockUpdate(core::ByteWriter &writer, ecs::Entity provider, const WeatherClock &clock) {
		writer.WriteString("weather.clock.anchor.v1");
		writer.WriteUInt64(provider.Id);
		WriteWeatherClock(writer, clock);
	}

	bool AcceptWeatherClockUpdate(
		ecs::Store &store, std::span<const std::byte> bytes, double nowSeconds, double roundTripSeconds
	) {
		core::ByteReader reader(bytes);
		if (reader.ReadString() != "weather.clock.anchor.v1") return false;
		const ecs::Entity provider{reader.ReadUInt64()};
		WeatherClock sample;
		if (!provider || !ReadWeatherClock(reader, sample) || reader.Remaining() != 0 ||
			!std::isfinite(nowSeconds) || !std::isfinite(roundTripSeconds) || roundTripSeconds < 0 ||
			roundTripSeconds > MAXIMUM_ROUND_TRIP_SECONDS)
			return true;
		const WeatherClockArrival arrival{provider, sample, nowSeconds, roundTripSeconds};
		(void)ApplyArrival(store, arrival, nowSeconds);
		WeatherClockInbox inbox;
		if (const auto *waiting = store.Resource<WeatherClockInbox>()) inbox = *waiting;
		if (RetainArrival(inbox, arrival, store)) store.SetResource(inbox);
		return true;
	}
}
