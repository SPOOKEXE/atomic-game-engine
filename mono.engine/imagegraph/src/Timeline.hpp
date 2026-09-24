#pragma once

// Private fixed-tick interpolation used by imagegraph evaluation and exact tests.

#include <engine/imagegraph/Document.hpp>

#include <cmath>
#include <cstdint>
#include <variant>

namespace engine::imagegraph::detail {
	inline uint8_t InterpolateChannel(uint8_t first, uint8_t last, uint64_t elapsed, uint64_t span) {
		const int64_t delta = static_cast<int64_t>(last) - first;
		const int64_t numerator = delta * static_cast<int64_t>(elapsed);
		const int64_t half = static_cast<int64_t>(span / 2);
		const int64_t rounded = numerator >= 0 ? (numerator + half) / static_cast<int64_t>(span)
											   : -((-numerator + half) / static_cast<int64_t>(span));
		return static_cast<uint8_t>(static_cast<int64_t>(first) + rounded);
	}

	// Linear interpolation is native and deliberately limited to numeric value kinds.
	inline Status
	Interpolate(const Value &first, const Value &last, uint64_t elapsed, uint64_t span, Value &result) {
		if (span == 0 || elapsed > span) return Status::InvalidValue;
		if (first.index() != last.index()) return Status::TypeMismatch;
		const double fraction = static_cast<double>(elapsed) / static_cast<double>(span);
		if (const auto *scalar = std::get_if<double>(&first)) {
			const double value = std::lerp(*scalar, std::get<double>(last), fraction);
			if (!std::isfinite(value)) return Status::InvalidValue;
			result = value;
			return Status::Ok;
		}
		if (const auto *vector = std::get_if<Vector2>(&first)) {
			const Vector2 end = std::get<Vector2>(last);
			const Vector2 value{std::lerp(vector->X, end.X, fraction), std::lerp(vector->Y, end.Y, fraction)};
			if (!std::isfinite(value.X) || !std::isfinite(value.Y)) return Status::InvalidValue;
			result = value;
			return Status::Ok;
		}
		if (const auto *colour = std::get_if<Colour>(&first)) {
			const Colour end = std::get<Colour>(last);
			result = Colour{
				InterpolateChannel(colour->Red, end.Red, elapsed, span),
				InterpolateChannel(colour->Green, end.Green, elapsed, span),
				InterpolateChannel(colour->Blue, end.Blue, elapsed, span),
				InterpolateChannel(colour->Alpha, end.Alpha, elapsed, span),
			};
			return Status::Ok;
		}
		return Status::UnsupportedExecution;
	}
}
