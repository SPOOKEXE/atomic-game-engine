#pragma once

// Parsing the machine-readable rows emitted by mono.build/benchmain.

#include <charconv>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace benchrunner {

	struct Measurement {
		std::string Suite;
		std::string Name;
		uint64_t Nanoseconds = 0;
		uint64_t Spread = 0;
		uint64_t Iterations = 0;
		std::string Unit;
	};

	inline bool ParseUnsigned(std::string_view text, uint64_t &value) {
		if (text.empty()) return false;
		const char *begin = text.data();
		const char *end = begin + text.size();
		const auto [at, error] = std::from_chars(begin, end, value);
		return error == std::errc{} && at == end;
	}

	inline std::vector<std::string_view> SplitBenchmarkFields(std::string_view text) {
		std::vector<std::string_view> fields;
		size_t begin = 0;
		while (begin <= text.size()) {
			const size_t end = text.find('\t', begin);
			if (end == std::string_view::npos) {
				fields.push_back(text.substr(begin));
				break;
			}
			fields.push_back(text.substr(begin, end - begin));
			begin = end + 1;
		}
		return fields;
	}

	// Parses every benchmark row for one selected suite. A successful benchmark
	// process that omitted its rows is a failed measurement, not a cacheable pass.
	inline bool ParseBenchmarkReport(
		std::string_view report,
		std::string_view expectedSuite,
		std::vector<Measurement> &into,
		std::string &error
	) {
		std::vector<Measurement> parsed;
		size_t lineBegin = 0;
		while (lineBegin < report.size()) {
			const size_t lineEnd = report.find('\n', lineBegin);
			const std::string_view line = report.substr(
				lineBegin, lineEnd == std::string_view::npos ? std::string_view::npos : lineEnd - lineBegin
			);
			lineBegin = lineEnd == std::string_view::npos ? report.size() : lineEnd + 1;
			if (!line.starts_with("bench\t")) continue;

			const std::vector<std::string_view> fields = SplitBenchmarkFields(line);
			if (fields.size() != 8 || fields[1] != expectedSuite ||
				(fields[6] != "call" && fields[6] != "item") || fields[7].empty()) {
				error = "malformed benchmark row: " + std::string(line);
				return false;
			}

			Measurement measurement;
			measurement.Suite = fields[1];
			if (!ParseUnsigned(fields[2], measurement.Nanoseconds) ||
				!ParseUnsigned(fields[3], measurement.Spread) ||
				!ParseUnsigned(fields[5], measurement.Iterations) || measurement.Iterations == 0) {
				error = "malformed benchmark row: " + std::string(line);
				return false;
			}
			uint64_t samples = 0;
			if (!ParseUnsigned(fields[4], samples) || samples == 0) {
				error = "malformed benchmark row: " + std::string(line);
				return false;
			}
			measurement.Unit = fields[6];
			measurement.Name = fields[7];
			parsed.push_back(std::move(measurement));
		}

		if (parsed.empty()) {
			error = "benchmark suite emitted no benchmark rows";
			return false;
		}
		into.insert(
			into.end(), std::make_move_iterator(parsed.begin()), std::make_move_iterator(parsed.end())
		);
		return true;
	}
}
