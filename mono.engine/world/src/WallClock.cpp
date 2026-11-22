#include <engine/core/Bytes.hpp>
#include <engine/core/Clock.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/world/WallClock.hpp>

#include <chrono>
#include <cmath>
#include <ctime>

namespace engine::world {

	namespace {
		// Days from 1970-01-01 to a proleptic Gregorian date. Howard Hinnant's
		// `days_from_civil`, so the offset below needs no `timegm`.
		int64_t DaysFromCivil(int64_t year, int64_t month, int64_t day) {
			year -= month <= 2 ? 1 : 0;
			const int64_t era = (year >= 0 ? year : year - 399) / 400;
			const int64_t yearOfEra = year - era * 400;
			const int64_t dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
			const int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
			return era * 146097 + dayOfEra - 719468;
		}

		// **The anchor and the zone, never `TickClock`.** The sample is the
		// machine's timer at the latest tick, and the next tick replaces it, so
		// a snapshot that carried it would differ between two runs that agree on
		// everything a world will do. A recording keeps the samples per barrier.
		void WriteWallClocks(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *clocks = static_cast<const WallClock *>(source);
			for (size_t index = 0; index < count; index++) {
				writer.WriteDouble(clocks[index].UnixEpoch);
				writer.WriteInt32(clocks[index].UtcOffsetSeconds);
			}
		}

		void ReadWallClocks(core::ByteReader &reader, void *destination, size_t count) {
			auto *clocks = static_cast<WallClock *>(destination);
			for (size_t index = 0; index < count; index++) {
				clocks[index] = WallClock{};
				clocks[index].UnixEpoch = reader.ReadDouble();
				clocks[index].UtcOffsetSeconds = reader.ReadInt32();
			}
		}
	}

	void RegisterWallClockTypes() {
		ecs::Components::Register<WallClock>("world.WallClock", WriteWallClocks, ReadWallClocks);
		ecs::Components::Register<ServerClock>("world.ServerClock");
		ecs::Components::Register<AuthorityTick>("world.AuthorityTick");
	}

	double SampleUnixSeconds() {
		const auto now = std::chrono::system_clock::now().time_since_epoch();
		const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
		return static_cast<double>(millis) / 1000.0;
	}

	double SampleClockSeconds() {
		return static_cast<double>(core::Clock::Nanoseconds()) / 1e9;
	}

	int32_t SampleUtcOffsetSeconds(double unixSeconds) {
		const std::time_t seconds = static_cast<std::time_t>(std::floor(unixSeconds));
		std::tm local{};
#if defined(_WIN32)
		if (localtime_s(&local, &seconds) != 0) return 0;
#else
		if (localtime_r(&seconds, &local) == nullptr) return 0;
#endif
		// The local wall reading, read back as if it were UTC, minus the instant.
		const int64_t asUtc = DaysFromCivil(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday) * 86400 +
							  local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
		return static_cast<int32_t>(asUtc - static_cast<int64_t>(seconds));
	}
}
