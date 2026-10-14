#include <engine/core/Bytes.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/weather/Events.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

TEST_SUITE_ID("engine.weather.events")

using namespace engine;
using Catch::Approx;

namespace {
	weather::StormEvent Event(uint64_t sequence = 1) {
		weather::StormEvent event;
		event.Layer = "Lighting/Storm";
		event.Id = {17, sequence, 9};
		event.Tick = sequence * 20;
		event.ElapsedSeconds = sequence;
		event.StartMetres = {100, 2000, 300};
		event.EndMetres = {100, 0, 300};
		event.BranchSeed = 81;
		event.Damage = 50;
		event.RadiusMetres = 8;
		return event;
	}
}

TEST_CASE("storm payload is named lossless and every truncated decode is transactional", "[weather]") {
	const auto event = Event();
	core::ByteWriter writer;
	weather::WriteStormEvent(writer, event);
	for (size_t size = 0; size < writer.Size(); ++size) {
		core::ByteReader reader(writer.Bytes().first(size));
		auto unchanged = Event(99);
		CHECK_FALSE(weather::ReadStormEvent(reader, unchanged));
		CHECK(unchanged == Event(99));
	}
	core::ByteReader reader(writer.Bytes());
	weather::StormEvent decoded;
	REQUIRE(weather::ReadStormEvent(reader, decoded));
	CHECK(reader.AtEnd());
	CHECK(decoded == event);
	CHECK(weather::StormEventKindName(decoded.Kind) == std::string_view("Lightning"));
	CHECK_FALSE(weather::ValidStormEvent(weather::StormEvent{}));
	decoded.StartMetres.X = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(weather::ValidStormEvent(decoded));
}

TEST_CASE(
	"bounded history and high-water cursors reject duplicates backwards ticks and old epochs", "[weather]"
) {
	weather::WeatherEvents events;
	for (uint64_t sequence = 1; sequence <= 40; ++sequence)
		REQUIRE(weather::AppendWeatherEvent(events, Event(sequence)));
	CHECK(events.Events.size() == weather::MAX_WEATHER_EVENTS);
	CHECK(events.Dropped == 8);
	CHECK(events.Events.front().Id.Sequence == 9);
	CHECK_FALSE(weather::AppendWeatherEvent(events, Event(40)));
	weather::WeatherEventCursor cursor;
	REQUIRE(weather::AcceptWeatherEvent(cursor, Event(40)));
	CHECK_FALSE(weather::AcceptWeatherEvent(cursor, Event(40)));
	CHECK_FALSE(weather::AcceptWeatherEvent(cursor, Event(39)));
	auto next = Event(41);
	next.Tick = 1;
	CHECK_FALSE(weather::AcceptWeatherEvent(cursor, next));
	next.Id.Epoch = 18;
	next.Id.Sequence = 1;
	REQUIRE(weather::AcceptWeatherEvent(cursor, next));
	CHECK_FALSE(weather::AcceptWeatherEvent(cursor, Event(100)));
	next.Layer = "different";
	CHECK_FALSE(weather::AcceptWeatherEvent(cursor, next));
}

TEST_CASE("thunder waits for listener distance with bounded capacity and no replay", "[weather]") {
	const auto speed = weather::SoundSpeedMetresPerSecond(20);
	REQUIRE(speed);
	CHECK(*speed == Approx(343.23).margin(0.1));
	CHECK_FALSE(weather::SoundSpeedMetresPerSecond(-273.15f));
	weather::ThunderQueue queue;
	const auto event = Event();
	REQUIRE(weather::QueueThunder(queue, event, {100, 0, 643.23f}, 20, event.Tick));
	CHECK(queue.Pending.front().DelaySeconds == Approx(1.0).margin(0.001));
	CHECK_FALSE(weather::QueueThunder(queue, event, {100, 0, 643.23f}, 20, event.Tick));
	std::array<weather::ThunderArrival, 1> arrivals;
	CHECK(weather::TakeDueThunder(queue, event.Tick + 19, arrivals) == 0);
	CHECK(weather::TakeDueThunder(queue, event.Tick + 21, arrivals) == 1);
	CHECK(arrivals.front().Event.Id == event.Id);
	CHECK(weather::TakeDueThunder(queue, event.Tick + 100, arrivals) == 0);
	auto hail = Event(2);
	hail.Kind = weather::StormEventKind::Hail;
	CHECK_FALSE(weather::QueueThunder(queue, hail, {}, 20, 0));
	for (uint64_t sequence = 2; sequence <= weather::MAX_WEATHER_EVENTS + 1; ++sequence)
		REQUIRE(weather::QueueThunder(queue, Event(sequence), {100, 0, 300}, 20, 0));
	CHECK_FALSE(weather::QueueThunder(queue, Event(100), {}, 20, 0));
	CHECK(weather::TakeDueThunder(queue, UINT64_MAX, arrivals) == 1);
	REQUIRE(weather::QueueThunder(queue, Event(100), {}, 20, 0));
}

TEST_CASE("thunder acoustic age survives rate edits skipped clock samples and huge world ages", "[weather]") {
	auto event = Event();
	event.Tick = UINT64_MAX;
	event.ElapsedSeconds = UINT64_MAX - 4;
	event.ElapsedFractionQ32 = 1u << 31u;
	const auto speed = weather::SoundSpeedMetresPerSecond(20);
	REQUIRE(speed);
	weather::ThunderQueue queue;
	REQUIRE(
		weather::QueueThunder(
			queue,
			event,
			{event.EndMetres.X + static_cast<float>(*speed * 2), 0, event.EndMetres.Z},
			20,
			event.Tick
		)
	);
	const auto &arrival = queue.Pending.front();
	CHECK_FALSE(weather::WeatherEventAgeSeconds(event, event.ElapsedSeconds - 1, 0));
	const auto age = weather::WeatherEventAgeSeconds(event, event.ElapsedSeconds + 1, 3u << 30u);
	REQUIRE(age);
	CHECK(*age == 1.25);
	CHECK_FALSE(weather::WeatherEventAgeSeconds(event, event.ElapsedSeconds, 0, -1));
	REQUIRE(weather::RemainingThunderSeconds(arrival, event.ElapsedSeconds, event.ElapsedFractionQ32));
	CHECK(
		*weather::RemainingThunderSeconds(arrival, event.ElapsedSeconds, event.ElapsedFractionQ32) ==
		Approx(2).margin(1e-6)
	);
	CHECK(
		*weather::RemainingThunderSeconds(arrival, event.ElapsedSeconds, 3u << 30u) ==
		Approx(1.75).margin(1e-6)
	);
	CHECK(
		*weather::RemainingThunderSeconds(arrival, event.ElapsedSeconds + 1, 3u << 30u) ==
		Approx(0.75).margin(1e-6)
	);
	// The consumer may skip any number of copied rate edits: only canonical elapsed contributes age.
	CHECK(*weather::RemainingThunderSeconds(arrival, event.ElapsedSeconds + 2, 3u << 30u) == 0);
	CHECK(
		*weather::RemainingThunderSeconds(
			arrival, event.ElapsedSeconds + 1, event.ElapsedFractionQ32, 0.25
		) == Approx(0.75).margin(1e-6)
	);
	CHECK(*weather::RemainingThunderSeconds(arrival, event.ElapsedSeconds - 1, 0) == arrival.DelaySeconds);
	CHECK_FALSE(weather::RemainingThunderSeconds(arrival, event.ElapsedSeconds, 0, -1));
	CHECK_FALSE(
		weather::RemainingThunderSeconds(
			arrival, event.ElapsedSeconds, 0, std::numeric_limits<double>::quiet_NaN()
		)
	);
	core::ByteWriter writer;
	weather::WriteStormEvent(writer, event);
	core::ByteReader reader(writer.Bytes());
	weather::StormEvent restored;
	REQUIRE(weather::ReadStormEvent(reader, restored));
	CHECK(restored == event);
}

TEST_CASE("listener queues separate layer clocks and clear replaced epochs", "[weather]") {
	weather::ThunderQueue queue;
	const auto first = Event();
	auto second = Event();
	second.Layer = "Lighting/OtherStorm";
	second.Tick = 2;
	second.TickRate = 100;
	REQUIRE(weather::QueueThunder(queue, first, first.EndMetres, 20, 0));
	REQUIRE(weather::QueueThunder(queue, second, second.EndMetres, 20, 0));
	std::array<weather::ThunderArrival, 1> output;
	CHECK(weather::TakeDueThunder(queue, 2, output, second.Layer) == 1);
	CHECK(output[0].Event.Layer == second.Layer);
	CHECK(queue.Pending.size() == 1);
	CHECK(weather::TakeDueThunder(queue, 2, output, first.Layer) == 0);
	CHECK(weather::TakeDueThunder(queue, 20, output, first.Layer) == 1);
	REQUIRE(weather::QueueThunder(queue, Event(2), first.StartMetres, 20, 0));
	CHECK(queue.Pending.front().DelaySeconds == 0);
	auto replacement = Event();
	replacement.Id.Epoch = 18;
	REQUIRE(weather::QueueThunder(queue, replacement, first.EndMetres, 20, 0));
	CHECK(queue.Pending.size() == 1);
	CHECK(queue.Pending.front().Event.Id.Epoch == 18);
}

TEST_CASE("lightning branches reconstruct from copied endpoints and seed with bounded output", "[weather]") {
	auto event = Event();
	std::array<weather::LightningSegment, weather::MAX_LIGHTNING_SEGMENTS> first, second;
	REQUIRE(weather::BuildLightningSegments(event, first) == first.size());
	REQUIRE(weather::BuildLightningSegments(event, second) == second.size());
	CHECK(first == second);
	CHECK(first.front().StartMetres == event.StartMetres);
	CHECK(first[15].EndMetres == event.EndMetres);
	for (uint32_t branch = 1; branch <= 4; ++branch) {
		const size_t start = 16 + static_cast<size_t>(branch - 1) * 3;
		CHECK(first[start].StartMetres == first[branch * 3].StartMetres);
		CHECK(first[start].Branch == branch);
		CHECK(first[start].EndMetres == first[start + 1].StartMetres);
		CHECK(first[start + 1].EndMetres == first[start + 2].StartMetres);
	}
	++event.BranchSeed;
	REQUIRE(weather::BuildLightningSegments(event, second) == second.size());
	CHECK(first != second);
	CHECK(first.front().StartMetres == second.front().StartMetres);
	CHECK(first[15].EndMetres == second[15].EndMetres);
	const auto unchanged = first;
	CHECK(weather::BuildLightningSegments(event, std::span(first).first(27)) == 0);
	CHECK(first == unchanged);
	event.Kind = weather::StormEventKind::Hail;
	CHECK(weather::BuildLightningSegments(event, first) == 0);
	event.Kind = weather::StormEventKind::Lightning;
	event.EndMetres = event.StartMetres;
	CHECK(weather::BuildLightningSegments(event, first) == 0);
}
