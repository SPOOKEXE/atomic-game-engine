#include <engine/core/Bytes.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/weather/Events.hpp>
#include <engine/weather/Weather.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace engine::weather {
	namespace {
		bool FinitePosition(core::Vector3 position) {
			return std::isfinite(position.X) && std::isfinite(position.Y) && std::isfinite(position.Z) &&
				   std::abs(position.X) <= 100000000.0f && std::abs(position.Y) <= 100000000.0f &&
				   std::abs(position.Z) <= 100000000.0f;
		}
		void WritePosition(core::ByteWriter &writer, core::Vector3 position) {
			writer.WriteFloat(position.X);
			writer.WriteFloat(position.Y);
			writer.WriteFloat(position.Z);
		}
		core::Vector3 ReadPosition(core::ByteReader &reader) {
			const float x = reader.ReadFloat();
			const float y = reader.ReadFloat();
			const float z = reader.ReadFloat();
			return {x, y, z};
		}
	}
	core::Vector3
	StormColumn(uint32_t seed, uint32_t layerKey, uint64_t tick, uint64_t epoch, core::Vector3 extentMetres) {
		const uint32_t columnSeed = Hash(
			seed ^ layerKey ^ Hash(static_cast<uint32_t>(tick)) ^ Hash(static_cast<uint32_t>(tick >> 32u)) ^
			Hash(static_cast<uint32_t>(epoch)) ^ Hash(static_cast<uint32_t>(epoch >> 32u))
		);
		const auto signedUnit = [](uint32_t random) {
			return static_cast<float>(random & 65535u) / 32767.5f - 1.0f;
		};
		return {
			signedUnit(Hash(columnSeed ^ 0x5853544fu)) * extentMetres.X * 0.5f,
			0.0f,
			signedUnit(Hash(columnSeed ^ 0x5a53544fu)) * extentMetres.Z * 0.5f
		};
	}
	const char *StormEventKindName(StormEventKind kind) {
		switch (kind) {
		case StormEventKind::Lightning:
			return "Lightning";
		case StormEventKind::Hail:
			return "Hail";
		}
		return "Invalid";
	}
	bool ValidStormEvent(const StormEvent &event) {
		return !event.Layer.empty() && event.Layer.size() <= MAX_WEATHER_LAYER_NAME_BYTES &&
			   event.Layer.find('\0') == std::string::npos && event.Id.Epoch != 0 && event.Id.Sequence != 0 &&
			   (event.Kind == StormEventKind::Lightning || event.Kind == StormEventKind::Hail) &&
			   event.TickRate > 0 && event.TickRate <= 1000 && FinitePosition(event.StartMetres) &&
			   FinitePosition(event.EndMetres) && std::isfinite(event.Damage) && event.Damage >= 0 &&
			   event.Damage <= 1000 && std::isfinite(event.RadiusMetres) && event.RadiusMetres >= 0 &&
			   event.RadiusMetres <= 1000;
	}
	void WriteStormEvent(core::ByteWriter &writer, const StormEvent &event) {
		writer.WriteUInt32(2);
		writer.WriteString(event.Layer);
		writer.WriteString(StormEventKindName(event.Kind));
		writer.WriteUInt64(event.Id.Epoch);
		writer.WriteUInt64(event.Id.Sequence);
		writer.WriteUInt32(event.Id.Seed);
		writer.WriteUInt64(event.Tick);
		writer.WriteUInt32(event.TickRate);
		writer.WriteUInt64(event.ElapsedSeconds);
		writer.WriteUInt32(event.ElapsedFractionQ32);
		WritePosition(writer, event.StartMetres);
		WritePosition(writer, event.EndMetres);
		writer.WriteUInt32(event.BranchSeed);
		writer.WriteFloat(event.Damage);
		writer.WriteFloat(event.RadiusMetres);
	}
	bool ReadStormEvent(core::ByteReader &reader, StormEvent &event) {
		const uint32_t version = reader.ReadUInt32();
		StormEvent decoded;
		const auto layer = reader.ReadString();
		if (layer.size() > MAX_WEATHER_LAYER_NAME_BYTES) {
			reader.Fail();
			return false;
		}
		decoded.Layer = layer;
		const auto kind = reader.ReadString();
		if (kind == "Lightning")
			decoded.Kind = StormEventKind::Lightning;
		else if (kind == "Hail")
			decoded.Kind = StormEventKind::Hail;
		else
			reader.Fail();
		decoded.Id.Epoch = reader.ReadUInt64();
		decoded.Id.Sequence = reader.ReadUInt64();
		decoded.Id.Seed = reader.ReadUInt32();
		decoded.Tick = reader.ReadUInt64();
		decoded.TickRate = reader.ReadUInt32();
		decoded.ElapsedSeconds = reader.ReadUInt64();
		decoded.ElapsedFractionQ32 = reader.ReadUInt32();
		decoded.StartMetres = ReadPosition(reader);
		decoded.EndMetres = ReadPosition(reader);
		decoded.BranchSeed = reader.ReadUInt32();
		decoded.Damage = reader.ReadFloat();
		decoded.RadiusMetres = reader.ReadFloat();
		if (reader.Failed() || version != 2 || !ValidStormEvent(decoded)) {
			reader.Fail();
			return false;
		}
		event = std::move(decoded);
		return true;
	}
	bool AppendWeatherEvent(WeatherEvents &events, const StormEvent &event) {
		if (!ValidStormEvent(event) || events.Events.size() > MAX_WEATHER_EVENTS) return false;
		if (!events.Events.empty()) {
			WeatherEventCursor cursor;
			if (!AcceptWeatherEvent(cursor, events.Events.back()) || !AcceptWeatherEvent(cursor, event))
				return false;
		}
		if (events.Events.size() == MAX_WEATHER_EVENTS) {
			events.Events.erase(events.Events.begin());
			if (events.Dropped != UINT64_MAX) ++events.Dropped;
		}
		events.Events.push_back(event);
		return true;
	}
	bool AcceptWeatherEvent(WeatherEventCursor &cursor, const StormEvent &event) {
		if (!ValidStormEvent(event)) return false;
		if (cursor.Ready && (cursor.Layer != event.Layer || event.Id.Epoch < cursor.Last.Epoch ||
							 (event.Id.Epoch == cursor.Last.Epoch &&
							  (event.Id.Sequence <= cursor.Last.Sequence || event.Tick < cursor.Tick))))
			return false;
		cursor = {event.Layer, event.Id, event.Tick, true};
		return true;
	}
	size_t BuildLightningSegments(const StormEvent &event, std::span<LightningSegment> output) {
		ENGINE_PROFILE("weather lightning branches");
		if (!ValidStormEvent(event) || event.Kind != StormEventKind::Lightning ||
			output.size() < MAX_LIGHTNING_SEGMENTS)
			return 0;
		constexpr uint32_t MAIN_SEGMENTS = 16;
		constexpr uint32_t BRANCHES = 4;
		constexpr uint32_t BRANCH_SEGMENTS = 3;
		static_assert(MAIN_SEGMENTS + BRANCHES * BRANCH_SEGMENTS == MAX_LIGHTNING_SEGMENTS);
		const auto chord = event.EndMetres - event.StartMetres;
		const float length = chord.Magnitude();
		if (!(length > 0)) return 0;
		const auto direction = chord / length;
		const auto perpendicular = [&](uint32_t purpose) {
			const auto signedUnit = [](uint32_t random) {
				return static_cast<float>(random & 65535u) / 32767.5f - 1.0f;
			};
			const uint32_t seed = Hash(event.BranchSeed ^ purpose);
			const core::Vector3 random{
				signedUnit(Hash(seed ^ 0x585a4947u)),
				signedUnit(Hash(seed ^ 0x595a4947u)),
				signedUnit(Hash(seed ^ 0x5a5a4947u))
			};
			return random - direction * random.Dot(direction);
		};
		std::array<core::Vector3, MAIN_SEGMENTS + 1> points;
		points.front() = event.StartMetres;
		points.back() = event.EndMetres;
		for (uint32_t index = 1; index < MAIN_SEGMENTS; ++index) {
			const float fraction = static_cast<float>(index) / MAIN_SEGMENTS;
			// Cosmetic chord jitter is two percent, tapered to keep the copied endpoints exact.
			points[index] =
				event.StartMetres + chord * fraction +
				perpendicular(Hash(index ^ 0x4d41494eu)) * (length * 0.02f * 4 * fraction * (1 - fraction));
		}
		for (uint32_t index = 0; index < MAIN_SEGMENTS; ++index)
			output[index] = {points[index], points[index + 1], 0};
		size_t written = MAIN_SEGMENTS;
		for (uint32_t branch = 1; branch <= BRANCHES; ++branch) {
			const auto start = points[branch * 3];
			const auto branchChord =
				direction * (length * 0.18f) + perpendicular(Hash(branch ^ 0x4252414eu)) * (length * 0.08f);
			core::Vector3 previous = start;
			for (uint32_t segment = 1; segment <= BRANCH_SEGMENTS; ++segment) {
				const float fraction = static_cast<float>(segment) / BRANCH_SEGMENTS;
				const auto next = start + branchChord * fraction +
								  perpendicular(Hash(branch * 31u + segment)) *
									  (length * 0.01f * 4 * fraction * (1 - fraction));
				output[written++] = {previous, next, branch};
				previous = next;
			}
		}
		return written;
	}
	std::optional<double> SoundSpeedMetresPerSecond(float temperatureCelsius) {
		if (!std::isfinite(temperatureCelsius) || temperatureCelsius < -100 || temperatureCelsius > 60)
			return std::nullopt;
		return std::sqrt(1.4 * 287.05 * (static_cast<double>(temperatureCelsius) + 273.15));
	}
	std::optional<double> WeatherEventAgeSeconds(
		const StormEvent &event,
		uint64_t elapsedSeconds,
		uint32_t elapsedFractionQ32,
		double fractionalSeconds
	) {
		if (!ValidStormEvent(event) || !std::isfinite(fractionalSeconds) || fractionalSeconds < 0 ||
			fractionalSeconds > 1)
			return std::nullopt;
		// Differences are formed before conversion; absolute world ages never become floating point.
		const double wholeAge = elapsedSeconds >= event.ElapsedSeconds
									? static_cast<double>(elapsedSeconds - event.ElapsedSeconds)
									: -static_cast<double>(event.ElapsedSeconds - elapsedSeconds);
		const int64_t fractionAge = static_cast<int64_t>(elapsedFractionQ32) - event.ElapsedFractionQ32;
		const double age = wholeAge + static_cast<double>(fractionAge) / 4294967296.0 + fractionalSeconds;
		return age < 0 ? std::nullopt : std::optional(age);
	}
	std::optional<double> RemainingThunderSeconds(
		const ThunderArrival &arrival,
		uint64_t elapsedSeconds,
		uint32_t elapsedFractionQ32,
		double fractionalSeconds
	) {
		if (!ValidStormEvent(arrival.Event) || !std::isfinite(arrival.DelaySeconds) ||
			arrival.DelaySeconds < 0 || !std::isfinite(fractionalSeconds) || fractionalSeconds < 0 ||
			fractionalSeconds > 1)
			return std::nullopt;
		const auto age =
			WeatherEventAgeSeconds(arrival.Event, elapsedSeconds, elapsedFractionQ32, fractionalSeconds);
		return age ? std::max(0.0, arrival.DelaySeconds - *age) : arrival.DelaySeconds;
	}
	bool QueueThunder(
		ThunderQueue &queue,
		const StormEvent &event,
		core::Vector3 listenerMetres,
		float temperatureCelsius,
		uint64_t listenerTick
	) {
		if (event.Kind != StormEventKind::Lightning || !ValidStormEvent(event) ||
			!FinitePosition(listenerMetres) || queue.Pending.size() > MAX_WEATHER_EVENTS ||
			queue.Cursors.size() > MAX_WEATHER_EVENTS)
			return false;
		const auto speed = SoundSpeedMetresPerSecond(temperatureCelsius);
		if (!speed) return false;
		auto existing = std::find_if(queue.Cursors.begin(), queue.Cursors.end(), [&](const auto &cursor) {
			return cursor.Layer == event.Layer;
		});
		if (existing == queue.Cursors.end() && queue.Cursors.size() == MAX_WEATHER_EVENTS) return false;
		WeatherEventCursor cursor = existing == queue.Cursors.end() ? WeatherEventCursor{} : *existing;
		if (!AcceptWeatherEvent(cursor, event)) return false;
		const auto replaced = [&](const ThunderArrival &arrival) {
			return arrival.Event.Layer == event.Layer && arrival.Event.Id.Epoch < event.Id.Epoch;
		};
		const auto retainedCount =
			std::count_if(queue.Pending.begin(), queue.Pending.end(), [&](const auto &arrival) {
				return !replaced(arrival);
			});
		if (retainedCount >= MAX_WEATHER_EVENTS) return false;

		const double channelX = static_cast<double>(event.StartMetres.X) - event.EndMetres.X;
		const double channelY = static_cast<double>(event.StartMetres.Y) - event.EndMetres.Y;
		const double channelZ = static_cast<double>(event.StartMetres.Z) - event.EndMetres.Z;
		const double listenerX = static_cast<double>(listenerMetres.X) - event.EndMetres.X;
		const double listenerY = static_cast<double>(listenerMetres.Y) - event.EndMetres.Y;
		const double listenerZ = static_cast<double>(listenerMetres.Z) - event.EndMetres.Z;
		const double channelLengthSquared = channelX * channelX + channelY * channelY + channelZ * channelZ;
		// Thunder starts at the nearest point of the declared straight channel; cosmetic branches are
		// ignored.
		const double fraction =
			channelLengthSquared == 0
				? 0.0
				: std::clamp(
					  (listenerX * channelX + listenerY * channelY + listenerZ * channelZ) /
						  channelLengthSquared,
					  0.0,
					  1.0
				  );
		const double x = listenerX - channelX * fraction;
		const double y = listenerY - channelY * fraction;
		const double z = listenerZ - channelZ * fraction;
		const double delay = std::sqrt(x * x + y * y + z * z) / *speed;
		const uint64_t delayTicks = static_cast<uint64_t>(std::ceil(delay * event.TickRate));
		// The acoustic pair still represents this arrival when the legacy tick due marker saturates.
		const uint64_t due = delayTicks > UINT64_MAX - event.Tick
								 ? UINT64_MAX
								 : std::max(listenerTick, event.Tick + delayTicks);
		if (existing == queue.Cursors.end())
			queue.Cursors.push_back(std::move(cursor));
		else
			*existing = std::move(cursor);
		std::erase_if(queue.Pending, replaced);
		queue.Pending.push_back({event, due, delay});
		std::stable_sort(queue.Pending.begin(), queue.Pending.end(), [](const auto &left, const auto &right) {
			return left.DueTick < right.DueTick;
		});
		return true;
	}
	size_t TakeDueThunder(
		ThunderQueue &queue, uint64_t listenerTick, std::span<ThunderArrival> output, std::string_view layer
	) {
		size_t count = 0;
		std::erase_if(queue.Pending, [&](const ThunderArrival &arrival) {
			if (count == output.size() || arrival.DueTick > listenerTick ||
				(!layer.empty() && arrival.Event.Layer != layer))
				return false;
			output[count++] = arrival;
			return true;
		});
		return count;
	}
}
