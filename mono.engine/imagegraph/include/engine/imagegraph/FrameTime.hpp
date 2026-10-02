#pragma once

// Canonical signed timeline positions retain exact integer magnitude and fractional identity.
#include <engine/imagegraph/Document.hpp>

#include <array>
#include <cmath>
#include <limits>

namespace engine::imagegraph {
	// Checks canonical fields and the caller's magnitude bound without converting Tick to a real.
	inline bool ValidFrameTime(const FrameTime &time, uint64_t maximum = Limits::MaximumTick) {
		return time.Tick <= maximum && (time.Tick != maximum || time.Subframe == 0) &&
			   std::isfinite(time.Subframe) && time.Subframe >= 0 && time.Subframe < 1 &&
			   !(time.NegativeFrame && time.Tick == 0 && time.Subframe == 0);
	}
	// Orders canonical positions exactly, including adjacent fractions near the uint64 maximum.
	inline int CompareFrameTime(const FrameTime &left, const FrameTime &right) {
		if (left.NegativeFrame != right.NegativeFrame) return left.NegativeFrame ? -1 : 1;
		const int magnitude = left.Tick != right.Tick			? (left.Tick < right.Tick ? -1 : 1)
							  : left.Subframe != right.Subframe ? (left.Subframe < right.Subframe ? -1 : 1)
																: 0;
		return left.NegativeFrame ? -magnitude : magnitude;
	}
	namespace frame_time_detail {
		inline int CompareMagnitude(const FrameTime &left, const FrameTime &right) {
			if (left.Tick != right.Tick) return left.Tick < right.Tick ? -1 : 1;
			return left.Subframe < right.Subframe ? -1 : left.Subframe > right.Subframe ? 1 : 0;
		}
		inline bool AddSigned(
			const FrameTime &left,
			bool leftNegative,
			const FrameTime &right,
			bool rightNegative,
			FrameTime &result
		) {
			if (leftNegative == rightNegative) {
				uint64_t whole = left.Tick + right.Tick;
				if (whole < left.Tick) return false;
				double fraction = left.Subframe + right.Subframe;
				if (fraction >= 1) {
					if (whole == ~uint64_t{0}) return false;
					++whole;
					fraction -= 1;
				}
				if (whole == ~uint64_t{0} && fraction != 0) return false;
				result = {whole, fraction, leftNegative && (whole != 0 || fraction != 0)};
				return true;
			}

			const int order = CompareMagnitude(left, right);
			if (order == 0) {
				result = {};
				return true;
			}
			const FrameTime &larger = order > 0 ? left : right;
			const FrameTime &smaller = order > 0 ? right : left;
			uint64_t whole = larger.Tick - smaller.Tick;
			double fraction = larger.Subframe - smaller.Subframe;
			if (fraction < 0) {
				--whole;
				fraction += 1;
				// A tiny subtraction can round the borrowed fraction back to one.
				if (fraction >= 1) {
					if (whole == ~uint64_t{0}) return false;
					++whole;
					fraction -= 1;
				}
			}
			const bool negative = order > 0 ? leftNegative : rightNegative;
			result = {whole, fraction, negative && (whole != 0 || fraction != 0)};
			return true;
		}
	}
	// Shifts a signed clock by the anchor delta, preserving tick and fraction components.
	// Inputs may use the full uint64 magnitude; the authored result remains bounded by MaximumTick.
	// Fraction operations use ordinary double rounding and normalize any rounded unit carry.
	inline bool ShiftFrameTime(
		const FrameTime &time,
		const FrameTime &oldAnchor,
		const FrameTime &newAnchor,
		FrameTime &result,
		bool clampZero = true
	) {
		constexpr uint64_t maximum = ~uint64_t{0};
		if (!ValidFrameTime(time, maximum) || !ValidFrameTime(oldAnchor, maximum) ||
			!ValidFrameTime(newAnchor, maximum))
			return false;

		const std::array<FrameTime, 3> terms{time, oldAnchor, newAnchor};
		const std::array<bool, 3> negative{
			time.NegativeFrame, !oldAnchor.NegativeFrame, newAnchor.NegativeFrame
		};
		std::array<bool, 3> used{};
		size_t positive = terms.size(), negativeTerm = terms.size();
		for (size_t index = 0; index < terms.size(); ++index) {
			if (terms[index].Tick == 0 && terms[index].Subframe == 0) continue;
			size_t &largest = negative[index] ? negativeTerm : positive;
			if (largest == terms.size() ||
				frame_time_detail::CompareMagnitude(terms[index], terms[largest]) > 0)
				largest = index;
		}
		if (positive == terms.size() && negativeTerm == terms.size()) {
			result = {};
			return true;
		}
		FrameTime accumulated{};
		bool accumulatedNegative = false;
		if (positive != terms.size() && negativeTerm != terms.size()) {
			if (!frame_time_detail::AddSigned(terms[positive], false, terms[negativeTerm], true, accumulated))
				return false;
			accumulatedNegative = accumulated.NegativeFrame;
			used[positive] = true;
			used[negativeTerm] = true;
		} else {
			const size_t first = positive != terms.size() ? positive : negativeTerm;
			accumulated = terms[first];
			accumulatedNegative = negative[first];
			accumulated.NegativeFrame = accumulatedNegative;
			used[first] = true;
		}
		for (size_t index = 0; index < terms.size(); ++index) {
			if (used[index] || (terms[index].Tick == 0 && terms[index].Subframe == 0)) continue;
			FrameTime sum;
			if (!frame_time_detail::AddSigned(
					accumulated, accumulatedNegative, terms[index], negative[index], sum
				)) {
				if (clampZero && accumulatedNegative && negative[index]) {
					result = {};
					return true;
				}
				return false;
			}
			accumulated = sum;
			accumulatedNegative = sum.NegativeFrame;
		}
		if (clampZero && accumulatedNegative) accumulated = {};
		if (!ValidFrameTime(accumulated)) return false;
		result = accumulated;
		return true;
	}
	inline long double FrameTimeDelta(const FrameTime &left, const FrameTime &right);
	// Scales a clock around a fixed anchor while retaining the integer tick separately.
	// Full uint64 inputs are accepted, but the scaled offset must fit uint64 before anchor addition.
	// This avoids converting ticks to double, at the cost of refusing some large cancelling offsets.
	// The published result is bounded by MaximumTick.
	// Exact anchors and endpoints keep their authored tuples; general fractional math rounds normally.
	inline bool ScaleFrameTime(
		const FrameTime &time,
		const FrameTime &fixedAnchor,
		const FrameTime &movingAnchor,
		const FrameTime &newEndpoint,
		FrameTime &result,
		bool clampZero = true
	) {
		constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
		if (!ValidFrameTime(time, maximum) || !ValidFrameTime(fixedAnchor, maximum) ||
			!ValidFrameTime(movingAnchor, maximum) || !ValidFrameTime(newEndpoint, maximum) ||
			CompareFrameTime(fixedAnchor, movingAnchor) == 0)
			return false;

		if (CompareFrameTime(time, fixedAnchor) == 0) {
			const FrameTime selected = clampZero && fixedAnchor.NegativeFrame ? FrameTime{} : fixedAnchor;
			if (!ValidFrameTime(selected)) return false;
			result = selected;
			return true;
		}
		if (CompareFrameTime(time, movingAnchor) == 0) {
			const FrameTime selected = clampZero && newEndpoint.NegativeFrame ? FrameTime{} : newEndpoint;
			if (!ValidFrameTime(selected)) return false;
			result = selected;
			return true;
		}
		if (CompareFrameTime(newEndpoint, movingAnchor) == 0) {
			const FrameTime selected = clampZero && time.NegativeFrame ? FrameTime{} : time;
			if (!ValidFrameTime(selected)) return false;
			result = selected;
			return true;
		}

		const long double denominator = FrameTimeDelta(movingAnchor, fixedAnchor);
		if (denominator == 0 || !std::isfinite(denominator)) return false;
		const long double factor = FrameTimeDelta(newEndpoint, fixedAnchor) / denominator;
		const long double scaled = FrameTimeDelta(time, fixedAnchor) * factor;
		if (!std::isfinite(factor) || !std::isfinite(scaled)) return false;
		if (scaled == 0) {
			const FrameTime selected = clampZero && fixedAnchor.NegativeFrame ? FrameTime{} : fixedAnchor;
			if (!ValidFrameTime(selected)) return false;
			result = selected;
			return true;
		}

		const long double magnitude = std::abs(scaled);
		long double whole = 0;
		const long double fraction = std::modf(magnitude, &whole);
		if (whole >= std::ldexp(1.0L, 64)) return false;
		uint64_t tick = static_cast<uint64_t>(whole);
		double subframe = static_cast<double>(fraction);
		if (!std::isfinite(subframe) || subframe < 0 || subframe > 1) return false;
		if (subframe == 1) {
			if (tick == std::numeric_limits<uint64_t>::max()) return false;
			++tick;
			subframe = 0;
		}
		const FrameTime offset{tick, subframe, scaled < 0 && (tick != 0 || subframe != 0)};
		FrameTime selected;
		if (!frame_time_detail::AddSigned(
				fixedAnchor, fixedAnchor.NegativeFrame, offset, offset.NegativeFrame, selected
			))
			return false;
		if (clampZero && selected.NegativeFrame) selected = {};
		if (!ValidFrameTime(selected)) return false;
		result = selected;
		return true;
	}
	// Subtracts whole magnitudes before conversion so nearby fractional positions retain their delta.
	inline long double FrameTimeDelta(const FrameTime &left, const FrameTime &right) {
		if (left.NegativeFrame != right.NegativeFrame) {
			const long double sum =
				static_cast<long double>(left.Tick) + right.Tick + left.Subframe + right.Subframe;
			return left.NegativeFrame ? -sum : sum;
		}
		const long double whole = left.Tick >= right.Tick ? static_cast<long double>(left.Tick - right.Tick)
														  : -static_cast<long double>(right.Tick - left.Tick);
		const long double difference = whole + static_cast<long double>(left.Subframe) - right.Subframe;
		return left.NegativeFrame ? -difference : difference;
	}
	// Display conversion is for bounded clocks; comparison and differences use the exact helpers above.
	inline long double FrameTimeToReal(const FrameTime &time) {
		const long double magnitude = static_cast<long double>(time.Tick) + time.Subframe;
		return time.NegativeFrame ? -magnitude : magnitude;
	}
	// Splits a finite real clock atomically; optional source rounding uses ties to even.
	inline bool SplitFrameTime(
		double frame, FrameTime &result, bool round = false, uint64_t maximum = Limits::MaximumTick
	) {
		if (!std::isfinite(frame)) return false;
		const long double magnitude = std::abs(static_cast<long double>(frame));
		long double whole = std::floor(magnitude);
		double fraction = static_cast<double>(magnitude - whole);
		if (whole > static_cast<long double>(maximum) || whole >= std::ldexp(1.0L, 64)) return false;
		if (round) {
			if (fraction > .5 || (fraction == .5 && static_cast<uint64_t>(whole) % 2)) ++whole;
			fraction = 0;
		}
		if (whole > static_cast<long double>(maximum) || whole >= std::ldexp(1.0L, 64)) return false;
		FrameTime selected{
			static_cast<uint64_t>(whole), fraction, frame < 0 && (whole != 0 || fraction != 0)
		};
		if (!ValidFrameTime(selected, maximum)) return false;
		result = selected;
		return true;
	}
	// Returns a nonowning numeric view without introducing another stored timestamp.
	inline FrameTime GetFrameTime(const Keyframe &key) {
		return {key.Tick, key.Subframe, key.NegativeFrame};
	}
	// Audio capture selection retains request.Tick independently of this signed authoring clock view.
	inline FrameTime GetFrameTime(const EvaluationRequest &request) {
		return {request.Tick, request.Subframe, request.NegativeFrame};
	}
	// Applies a validated clock atomically to the existing key fields.
	inline bool SetFrameTime(Keyframe &key, const FrameTime &time) {
		if (!ValidFrameTime(time)) return false;
		key.Tick = time.Tick;
		key.Subframe = time.Subframe;
		key.NegativeFrame = time.NegativeFrame;
		return true;
	}
	// Applies a validated signed authoring clock without changing captured input sources.
	inline bool SetFrameTime(EvaluationRequest &request, const FrameTime &time) {
		if (!ValidFrameTime(time)) return false;
		request.Tick = time.Tick;
		request.Subframe = time.Subframe;
		request.NegativeFrame = time.NegativeFrame;
		return true;
	}
}
