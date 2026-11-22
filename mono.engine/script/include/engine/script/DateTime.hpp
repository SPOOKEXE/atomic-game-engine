#pragma once

// Dates for scripts: the clock they read and the calendar maths both VMs share.
//
// **Every date a script reads comes from `world::WallClock`**, an anchor the
// world took once, plus simulated time. So `os.time()`, `DateTime.now()` and
// `Workspace:GetServerTimeNow()` replay. See `world/WallClock.hpp` for why.
//
// A plugin's clock follows its beats rather than a world, and a plugin is not
// recorded, so it reads the machine's time at each beat instead. See
// `TaskClock::Beat`.
//
// The calendar is proleptic Gregorian over Unix milliseconds, with the range
// Roblox's `DateTime` has: the years 1400 through 9999. Names and formats are
// en-us, the only locale this engine carries.
//
// @tier L9 · shared

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace engine::ecs {
	class Store;
}

namespace engine::script {

	class TaskClock;

	// Unix seconds now, as this world's scripts see it.
	//
	// A replica reads the authority's `world::ServerClock`. An authority reads
	// its anchor plus its simulated elapsed time. A plugin reads its last beat.
	double UnixNow(const ecs::Store &store, const TaskClock &clock);

	// The real timer sample of the current tick, in seconds. What `os.clock()`
	// returns: every script in one tick reads the same value.
	double ClockNow(const ecs::Store &store, const TaskClock &clock);

	// Seconds east of UTC that local time is, for this world's scripts.
	int32_t UtcOffsetNow(const ecs::Store &store, const TaskClock &clock);

	namespace datetime {
		// A date split into fields. `Weekday` is 0 for Sunday, `YearDay` is 1
		// for January 1st.
		struct Civil {
			int64_t Year = 1970;
			int Month = 1;
			int Day = 1;
			int Hour = 0;
			int Minute = 0;
			int Second = 0;
			int Millisecond = 0;
			int Weekday = 4;
			int YearDay = 1;
		};

		// The first and last millisecond `DateTime` holds: 1400-01-01 and
		// 9999-12-31T23:59:59.999, both UTC.
		extern const int64_t MIN_MILLIS;
		extern const int64_t MAX_MILLIS;

		// Whether `millis` is inside the range above.
		bool InRange(int64_t millis);

		// The UTC fields of a Unix millisecond.
		Civil ToCivil(int64_t millis);

		// The Unix millisecond of a set of fields. Fields past their range roll
		// over into the next unit, so month 13 is January of the next year.
		int64_t FromCivil(
			int64_t year,
			int64_t month,
			int64_t day,
			int64_t hour,
			int64_t minute,
			int64_t second,
			int64_t millisecond
		);

		// `floor(millis / 1000)`, which is what `UnixTimestamp` reads.
		int64_t FloorSeconds(int64_t millis);

		// `YYYY-MM-DDTHH:MM:SSZ`, which is what `DateTime:ToIsoDate()` returns.
		std::string ToIso(int64_t millis);

		// Reads an ISO 8601 date: `YYYY-MM-DD`, then optionally `T` and
		// `HH:MM`, `:SS` and a fraction, then optionally `Z` or `+HH:MM`. No zone
		// is UTC. Nothing on malformed text or a date out of range.
		std::optional<int64_t> ParseIso(std::string_view text);

		// `DateTime:FormatUniversalTime` and `FormatLocalTime`: the moment.js
		// tokens Roblox uses, such as `YYYY`, `MMMM`, `Do`, `HH`, `A` and the
		// composites `LT` through `llll`, with `[...]` for literal text.
		// `millis` is already shifted into the zone being shown.
		std::string Format(int64_t millis, std::string_view format);

		// `os.date`'s `strftime` conversions over a shifted millisecond, with
		// `offsetSeconds` for `%z` and `%Z`. Sets `error` and returns empty for
		// a conversion Luau does not allow.
		std::string
		StrFTime(int64_t millis, std::string_view format, int32_t offsetSeconds, std::string &error);
	}
}
