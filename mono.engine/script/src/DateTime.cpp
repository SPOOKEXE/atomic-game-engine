#include <engine/ecs/Store.hpp>
#include <engine/script/DateTime.hpp>
#include <engine/script/Tasks.hpp>
#include <engine/world/Postbox.hpp>
#include <engine/world/WallClock.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace engine::script {

	double UnixNow(const ecs::Store &store, const TaskClock &clock) {
		if (clock.FollowsBeats()) return clock.BeatUnixSeconds();
		const world::Replica *replica = store.Resource<world::Replica>();
		if (replica != nullptr && replica->Active) {
			const auto *server = store.Resource<world::ServerClock>();
			const auto *tick = store.Resource<world::AuthorityTick>();
			if (server != nullptr && tick != nullptr) {
				return server->UnixAtTickZero + static_cast<double>(tick->Tick) * server->TickDelta +
					   (store.Time().Elapsed - tick->LocalElapsed);
			}
		}
		const world::WallClock *anchor = store.Resource<world::WallClock>();
		return (anchor != nullptr ? anchor->UnixEpoch : 0.0) + store.Time().Elapsed;
	}

	double ClockNow(const ecs::Store &store, const TaskClock &clock) {
		if (clock.FollowsBeats()) return clock.BeatClockSeconds();
		const world::WallClock *anchor = store.Resource<world::WallClock>();
		return anchor != nullptr ? anchor->TickClock : 0.0;
	}

	int32_t UtcOffsetNow(const ecs::Store &store, const TaskClock &clock) {
		if (clock.FollowsBeats()) return clock.BeatUtcOffsetSeconds();
		const world::WallClock *anchor = store.Resource<world::WallClock>();
		return anchor != nullptr ? anchor->UtcOffsetSeconds : 0;
	}

	namespace datetime {
		namespace {
			constexpr int64_t MILLIS_PER_DAY = 86'400'000;

			constexpr std::array<const char *, 12> MONTHS = {
				"January",
				"February",
				"March",
				"April",
				"May",
				"June",
				"July",
				"August",
				"September",
				"October",
				"November",
				"December"
			};
			constexpr std::array<const char *, 7> WEEKDAYS = {
				"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
			};

			int64_t FloorDiv(int64_t value, int64_t by) {
				const int64_t quotient = value / by;
				return (value % by != 0 && (value < 0) != (by < 0)) ? quotient - 1 : quotient;
			}

			// Howard Hinnant's `days_from_civil` and `civil_from_days`.
			int64_t DaysFromCivil(int64_t year, int64_t month, int64_t day) {
				year -= month <= 2 ? 1 : 0;
				const int64_t era = FloorDiv(year, 400);
				const int64_t yearOfEra = year - era * 400;
				const int64_t dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
				const int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
				return era * 146097 + dayOfEra - 719468;
			}

			void CivilFromDays(int64_t days, int64_t &year, int &month, int &day) {
				days += 719468;
				const int64_t era = FloorDiv(days, 146097);
				const int64_t dayOfEra = days - era * 146097;
				const int64_t yearOfEra =
					(dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
				const int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
				const int64_t shifted = (5 * dayOfYear + 2) / 153;
				day = static_cast<int>(dayOfYear - (153 * shifted + 2) / 5 + 1);
				month = static_cast<int>(shifted < 10 ? shifted + 3 : shifted - 9);
				year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0);
			}

			std::string Padded(int64_t value, int width) {
				char buffer[32];
				std::snprintf(buffer, sizeof(buffer), "%0*lld", width, static_cast<long long>(value));
				return buffer;
			}

			std::string Ordinal(int64_t value) {
				const int64_t tens = value % 100;
				const char *suffix = "th";
				if (tens < 11 || tens > 13) {
					switch (value % 10) {
					case 1:
						suffix = "st";
						break;
					case 2:
						suffix = "nd";
						break;
					case 3:
						suffix = "rd";
						break;
					default:
						break;
					}
				}
				return std::to_string(value) + suffix;
			}

			int Hour12(int hour) {
				const int twelve = hour % 12;
				return twelve == 0 ? 12 : twelve;
			}

			// The en-us expansions of moment.js's localized composites.
			std::string_view Composite(std::string_view token) {
				if (token == "LTS") return "h:mm:ss A";
				if (token == "LT") return "h:mm A";
				if (token == "LLLL") return "dddd, MMMM D, YYYY h:mm A";
				if (token == "LLL") return "MMMM D, YYYY h:mm A";
				if (token == "LL") return "MMMM D, YYYY";
				if (token == "L") return "MM/DD/YYYY";
				if (token == "llll") return "ddd, MMM D, YYYY h:mm A";
				if (token == "lll") return "MMM D, YYYY h:mm A";
				if (token == "ll") return "MMM D, YYYY";
				if (token == "l") return "M/D/YYYY";
				return {};
			}

			// Longest first, so `MMMM` is not read as `MM` twice.
			constexpr std::array<std::string_view, 46> TOKENS = {
				"LLLL", "llll", "YYYY", "MMMM", "DDDD", "dddd", "SSSS", "LTS", "LLL", "lll", "MMM", "DDDo",
				"DDD",	"ddd",	"SSS",	"YY",	"Qo",	"Mo",	"MM",	"Do",  "DD",  "do",	 "dd",	"HH",
				"hh",	"kk",	"mm",	"ss",	"SS",	"LT",	"LL",	"ll",  "Q",	  "M",	 "D",	"d",
				"H",	"h",	"k",	"m",	"s",	"S",	"A",	"a",   "X",	  "x",
			};

			std::string Expand(int64_t millis, std::string_view token) {
				const Civil c = ToCivil(millis);
				if (token == "YYYY") return Padded(c.Year, 4);
				if (token == "YY") return Padded(((c.Year % 100) + 100) % 100, 2);
				if (token == "Q") return std::to_string((c.Month - 1) / 3 + 1);
				if (token == "Qo") return Ordinal((c.Month - 1) / 3 + 1);
				if (token == "M") return std::to_string(c.Month);
				if (token == "Mo") return Ordinal(c.Month);
				if (token == "MM") return Padded(c.Month, 2);
				if (token == "MMM") return std::string(MONTHS[c.Month - 1]).substr(0, 3);
				if (token == "MMMM") return MONTHS[c.Month - 1];
				if (token == "D") return std::to_string(c.Day);
				if (token == "Do") return Ordinal(c.Day);
				if (token == "DD") return Padded(c.Day, 2);
				if (token == "DDD") return std::to_string(c.YearDay);
				if (token == "DDDo") return Ordinal(c.YearDay);
				if (token == "DDDD") return Padded(c.YearDay, 3);
				if (token == "d") return std::to_string(c.Weekday);
				if (token == "do") return Ordinal(c.Weekday);
				if (token == "dd") return std::string(WEEKDAYS[c.Weekday]).substr(0, 2);
				if (token == "ddd") return std::string(WEEKDAYS[c.Weekday]).substr(0, 3);
				if (token == "dddd") return WEEKDAYS[c.Weekday];
				if (token == "H") return std::to_string(c.Hour);
				if (token == "HH") return Padded(c.Hour, 2);
				if (token == "h") return std::to_string(Hour12(c.Hour));
				if (token == "hh") return Padded(Hour12(c.Hour), 2);
				if (token == "k") return std::to_string(c.Hour == 0 ? 24 : c.Hour);
				if (token == "kk") return Padded(c.Hour == 0 ? 24 : c.Hour, 2);
				if (token == "m") return std::to_string(c.Minute);
				if (token == "mm") return Padded(c.Minute, 2);
				if (token == "s") return std::to_string(c.Second);
				if (token == "ss") return Padded(c.Second, 2);
				if (token == "A") return c.Hour < 12 ? "AM" : "PM";
				if (token == "a") return c.Hour < 12 ? "am" : "pm";
				if (token == "X") return std::to_string(FloorSeconds(millis));
				if (token == "x") return std::to_string(millis);
				if (token[0] == 'S') {
					// Fractional seconds to the width asked for, truncated.
					const std::string three = Padded(c.Millisecond, 3);
					return token.size() <= 3 ? three.substr(0, token.size())
											 : three + std::string(token.size() - 3, '0');
				}
				return std::string(token);
			}

			bool Digits(std::string_view text, size_t at, size_t count, int64_t &out) {
				if (at + count > text.size()) return false;
				out = 0;
				for (size_t index = at; index < at + count; index++) {
					if (text[index] < '0' || text[index] > '9') return false;
					out = out * 10 + (text[index] - '0');
				}
				return true;
			}
		}

		const int64_t MIN_MILLIS = DaysFromCivil(1400, 1, 1) * MILLIS_PER_DAY;
		const int64_t MAX_MILLIS = DaysFromCivil(10000, 1, 1) * MILLIS_PER_DAY - 1;

		bool InRange(int64_t millis) {
			return millis >= MIN_MILLIS && millis <= MAX_MILLIS;
		}

		int64_t FloorSeconds(int64_t millis) {
			return FloorDiv(millis, 1000);
		}

		Civil ToCivil(int64_t millis) {
			Civil civil;
			const int64_t days = FloorDiv(millis, MILLIS_PER_DAY);
			const int64_t ofDay = millis - days * MILLIS_PER_DAY;
			CivilFromDays(days, civil.Year, civil.Month, civil.Day);
			civil.Hour = static_cast<int>(ofDay / 3'600'000);
			civil.Minute = static_cast<int>(ofDay / 60'000 % 60);
			civil.Second = static_cast<int>(ofDay / 1000 % 60);
			civil.Millisecond = static_cast<int>(ofDay % 1000);
			// 1970-01-01 was a Thursday.
			civil.Weekday = static_cast<int>(((days % 7) + 11) % 7);
			civil.YearDay = static_cast<int>(days - DaysFromCivil(civil.Year, 1, 1) + 1);
			return civil;
		}

		int64_t FromCivil(
			int64_t year,
			int64_t month,
			int64_t day,
			int64_t hour,
			int64_t minute,
			int64_t second,
			int64_t millisecond
		) {
			// Months roll over into years first; everything below a month is a
			// plain sum of milliseconds, which rolls over by itself.
			const int64_t monthIndex = month - 1;
			year += FloorDiv(monthIndex, 12);
			month = monthIndex - FloorDiv(monthIndex, 12) * 12 + 1;
			const int64_t days = DaysFromCivil(year, month, 1) + (day - 1);
			return days * MILLIS_PER_DAY + hour * 3'600'000 + minute * 60'000 + second * 1000 + millisecond;
		}

		std::string ToIso(int64_t millis) {
			const Civil c = ToCivil(millis);
			return Padded(c.Year, 4) + "-" + Padded(c.Month, 2) + "-" + Padded(c.Day, 2) + "T" +
				   Padded(c.Hour, 2) + ":" + Padded(c.Minute, 2) + ":" + Padded(c.Second, 2) + "Z";
		}

		std::optional<int64_t> ParseIso(std::string_view text) {
			int64_t year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, millisecond = 0;
			if (!Digits(text, 0, 4, year) || text.size() < 10 || text[4] != '-' ||
				!Digits(text, 5, 2, month) || text[7] != '-' || !Digits(text, 8, 2, day))
				return std::nullopt;
			size_t at = 10;
			if (at < text.size() && (text[at] == 'T' || text[at] == 't' || text[at] == ' ')) {
				if (!Digits(text, at + 1, 2, hour) || at + 3 >= text.size() || text[at + 3] != ':' ||
					!Digits(text, at + 4, 2, minute))
					return std::nullopt;
				at += 6;
				if (at < text.size() && text[at] == ':') {
					if (!Digits(text, at + 1, 2, second)) return std::nullopt;
					at += 3;
					if (at < text.size() && (text[at] == '.' || text[at] == ',')) {
						at++;
						int64_t scale = 100;
						const size_t start = at;
						while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
							millisecond += (text[at] - '0') * scale;
							scale /= 10;
							at++;
						}
						if (at == start) return std::nullopt;
					}
				}
			}
			int64_t offsetMinutes = 0;
			if (at < text.size()) {
				if (text[at] == 'Z' || text[at] == 'z') {
					at++;
				} else if (text[at] == '+' || text[at] == '-') {
					const int64_t sign = text[at] == '-' ? -1 : 1;
					int64_t hours = 0, minutes = 0;
					if (!Digits(text, at + 1, 2, hours)) return std::nullopt;
					at += 3;
					if (at < text.size() && text[at] == ':') at++;
					if (at < text.size()) {
						if (!Digits(text, at, 2, minutes)) return std::nullopt;
						at += 2;
					}
					offsetMinutes = sign * (hours * 60 + minutes);
				}
			}
			if (at != text.size()) return std::nullopt;
			if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60)
				return std::nullopt;
			const int64_t millis =
				FromCivil(year, month, day, hour, minute, second, millisecond) - offsetMinutes * 60'000;
			if (!InRange(millis) || ToCivil(millis + offsetMinutes * 60'000).Day != day) return std::nullopt;
			return millis;
		}

		std::string Format(int64_t millis, std::string_view format) {
			std::string out;
			size_t at = 0;
			while (at < format.size()) {
				if (format[at] == '[') {
					const size_t close = format.find(']', at + 1);
					if (close != std::string_view::npos) {
						out.append(format.substr(at + 1, close - at - 1));
						at = close + 1;
						continue;
					}
				}
				bool matched = false;
				for (const std::string_view token : TOKENS) {
					if (format.substr(at, token.size()) != token) continue;
					const std::string_view composite = Composite(token);
					out += composite.empty() ? Expand(millis, token) : Format(millis, composite);
					at += token.size();
					matched = true;
					break;
				}
				if (!matched) out += format[at++];
			}
			return out;
		}

		std::string
		StrFTime(int64_t millis, std::string_view format, int32_t offsetSeconds, std::string &error) {
			const Civil c = ToCivil(millis);
			const int mondayFirst = (c.Weekday + 6) % 7;
			const std::string zone = [&] {
				const int32_t minutes = std::abs(offsetSeconds) / 60;
				return std::string(offsetSeconds < 0 ? "-" : "+") + Padded(minutes / 60, 2) +
					   Padded(minutes % 60, 2);
			}();
			std::string out;
			for (size_t at = 0; at < format.size(); at++) {
				if (format[at] != '%') {
					out += format[at];
					continue;
				}
				if (++at >= format.size()) {
					error = "invalid conversion specifier '%'";
					return {};
				}
				switch (format[at]) {
				case 'a':
					out += std::string(WEEKDAYS[c.Weekday]).substr(0, 3);
					break;
				case 'A':
					out += WEEKDAYS[c.Weekday];
					break;
				case 'b':
					out += std::string(MONTHS[c.Month - 1]).substr(0, 3);
					break;
				case 'B':
					out += MONTHS[c.Month - 1];
					break;
				case 'c':
					out += StrFTime(millis, "%a %b %d %H:%M:%S %Y", offsetSeconds, error);
					break;
				case 'd':
					out += Padded(c.Day, 2);
					break;
				case 'H':
					out += Padded(c.Hour, 2);
					break;
				case 'I':
					out += Padded(Hour12(c.Hour), 2);
					break;
				case 'j':
					out += Padded(c.YearDay, 3);
					break;
				case 'm':
					out += Padded(c.Month, 2);
					break;
				case 'M':
					out += Padded(c.Minute, 2);
					break;
				case 'p':
					out += c.Hour < 12 ? "AM" : "PM";
					break;
				case 'S':
					out += Padded(c.Second, 2);
					break;
				case 'U':
					out += Padded((c.YearDay - 1 + 7 - c.Weekday) / 7, 2);
					break;
				case 'w':
					out += std::to_string(c.Weekday);
					break;
				case 'W':
					out += Padded((c.YearDay - 1 + 7 - mondayFirst) / 7, 2);
					break;
				case 'x':
					out += StrFTime(millis, "%m/%d/%y", offsetSeconds, error);
					break;
				case 'X':
					out += StrFTime(millis, "%H:%M:%S", offsetSeconds, error);
					break;
				case 'y':
					out += Padded(((c.Year % 100) + 100) % 100, 2);
					break;
				case 'Y':
					out += std::to_string(c.Year);
					break;
				case 'z':
					out += zone;
					break;
				case 'Z':
					out += offsetSeconds == 0 ? std::string("UTC") : zone;
					break;
				case '%':
					out += '%';
					break;
				default:
					error = std::string("invalid conversion specifier '%") + format[at] + "'";
					return {};
				}
			}
			return out;
		}
	}
}
