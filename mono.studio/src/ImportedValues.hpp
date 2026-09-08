#pragma once

#include <engine/game/Values.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace studio {

	// Preserve integer payloads before narrowing, and reject values the declared type cannot hold.
	template <typename Number>
	bool ReadImportedNumber(engine::ecs::PropertyType type, Number number, engine::game::PropertyValue &out) {
		using engine::ecs::PropertyType;
		if constexpr (std::is_floating_point_v<Number>) {
			if (!std::isfinite(number)) {
				return false;
			}
		}
		switch (type) {
		case PropertyType::Int32:
		case PropertyType::Int64:
			if constexpr (std::is_integral_v<Number>) {
				if (type == PropertyType::Int32) {
					if (!std::in_range<int32_t>(number)) {
						return false;
					}
					out.Int32 = static_cast<int32_t>(number);
				} else {
					if (!std::in_range<int64_t>(number)) {
						return false;
					}
					out.Int64 = static_cast<int64_t>(number);
				}
			} else {
				const double limit = type == PropertyType::Int32 ? 2147483648.0 : 9223372036854775808.0;
				if (number < -limit || number >= limit || std::trunc(number) != number) {
					return false;
				}
				if (type == PropertyType::Int32) {
					out.Int32 = static_cast<int32_t>(number);
				} else {
					out.Int64 = static_cast<int64_t>(number);
				}
			}
			return true;
		case PropertyType::Float:
			if (static_cast<long double>(number) < -std::numeric_limits<float>::max() ||
				static_cast<long double>(number) > std::numeric_limits<float>::max()) {
				return false;
			}
			out.Float = static_cast<float>(number);
			return true;
		case PropertyType::Double:
			out.Double = static_cast<double>(number);
			return true;
		default:
			return false;
		}
	}
	// Sequences remain in authored order. Reject invalid stops instead of sorting or truncating them.
	template <typename Points, typename Sequence>
	bool ReadImportedSequence(const Points &points, Sequence &out) {
		if (points.size() > engine::core::SEQUENCE_CAPACITY) {
			return false;
		}
		float previous = 0;
		for (const auto &point : points) {
			if (!std::isfinite(point.Time) || point.Time < previous || point.Time > 1.0f) {
				return false;
			}
			if constexpr (std::is_same_v<Sequence, engine::core::NumberSequence>) {
				if (!std::isfinite(point.Value) || !std::isfinite(point.Envelope) || point.Envelope < 0) {
					return false;
				}
			} else {
				if (!std::isfinite(point.Value.R) || !std::isfinite(point.Value.G) ||
					!std::isfinite(point.Value.B)) {
					return false;
				}
			}
			previous = point.Time;
		}
		out = Sequence{};
		for (const auto &point : points) {
			out.Add(point);
		}
		return true;
	}

}
