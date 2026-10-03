#pragma once

// Persisted gradient objects use int64 packed colours and retain all 128 CPU
// keys.
#include "nodes/Gradient.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <string_view>

namespace engine::imagegraph::detail {
	inline uint8_t SourceGradientByte(double value) {
		value = std::clamp(value, 0.0, 255.0);
		const double lower = std::floor(value), fraction = value - lower;
		return static_cast<uint8_t>(
			lower + (fraction > .5 || (fraction == .5 && std::fmod(lower, 2.0) != 0))
		);
	}
	inline Rgb3 CpuOklabMix(const Rgb3 &a, const Rgb3 &b, double amount) {
		constexpr std::array<Rgb3, 3> TO{
			{{.4121656120, .2118591070, .0883097947},
			 {.5362752080, .6807189584, .2818474174},
			 {.0514575653, .1074065790, .6302613616}}
		};
		constexpr std::array<Rgb3, 3> FROM{
			{{4.0767245293, -1.2681437731, -.0041119885},
			 {-3.3072168827, 2.6093323231, -.7034763098},
			 {.2307590544, -.3411344290, 1.7068625689}}
		};
		Rgb3 mixed{}, result{};
		for (size_t row = 0; row < 3; row++) {
			double x = 0, y = 0;
			for (size_t channel = 0; channel < 3; channel++) {
				x += TO[row][channel] * std::pow(a[channel], 2.2);
				y += TO[row][channel] * std::pow(b[channel], 2.2);
			}
			mixed[row] = std::pow(x, 1.0 / 3) + (std::pow(y, 1.0 / 3) - std::pow(x, 1.0 / 3)) * amount;
		}
		for (size_t row = 0; row < 3; row++) {
			double linear = 0;
			for (size_t channel = 0; channel < 3; channel++)
				linear += FROM[row][channel] * std::pow(mixed[channel], 3);
			result[row] = std::pow(std::max(0.0, linear), 1.0 / 2.2);
		}
		return result;
	}

	inline bool SampleSourceGradient(
		const Gradient &gradient, double position, Colour &output, std::string_view &failure
	) {
		if (!std::isfinite(position)) return (failure = "gradient sample must be finite", false);
		const auto &keys = gradient.Keys;
		if (keys.empty()) {
			output = {0, 0, 0, 0};
			return true;
		}
		if (keys.size() == 1 || position <= keys.front().Time) {
			output = keys.front().Color;
			return true;
		}
		if (position >= keys.back().Time) {
			output = keys.back().Color;
			return true;
		}
		for (size_t index = 1; index < keys.size(); index++) {
			const auto &before = keys[index - 1], &after = keys[index];
			if (after.Time < position) continue;
			if (after.Time == position) {
				output = after.Color;
				return true;
			}
			if (gradient.Mode == 1) {
				output = before.Color;
				return true;
			}
			const double amount = (position - before.Time) / (after.Time - before.Time);
			if (!std::isfinite(amount)) return (failure = "gradient interpolation is not finite", false);
			const Colour &a = before.Color, &b = after.Color;
			Rgb3 c0{a.Red / 255.0, a.Green / 255.0, a.Blue / 255.0};
			Rgb3 c1{b.Red / 255.0, b.Green / 255.0, b.Blue / 255.0};
			Rgb3 mixed;
			if (gradient.Mode == 2 || gradient.Mode == 5) {
				// GML hue/saturation/value getters quantize to bytes before
				// interpolation.
				auto h0 = ShaderRgbToHsv(c0), h1 = ShaderRgbToHsv(c1);
				for (size_t channel = 0; channel < 3; channel++) {
					h0[channel] = SourceGradientByte(h0[channel] * 255) / 255.0;
					h1[channel] = SourceGradientByte(h1[channel] * 255) / 255.0;
				}
				// GML frac preserves the sign of its input, unlike GLSL fract.
				const double delta = std::fmod(h1[0] - h0[0], 1.0);
				double distance = std::fmod(2 * delta, 1.0) - delta;
				if (gradient.Mode == 5) distance -= (distance > 0) - (distance < 0);
				mixed = ShaderHsvToRgb(
					{SourceGradientByte((h0[0] + distance * amount) * 255) / 255.0,
					 SourceGradientByte((h0[1] + (h1[1] - h0[1]) * amount) * 255) / 255.0,
					 SourceGradientByte((h0[2] + (h1[2] - h0[2]) * amount) * 255) / 255.0}
				);
			} else if (gradient.Mode == 3)
				mixed = CpuOklabMix(c0, c1, amount);
			else
				mixed = GradientMix(c0, c1, amount, gradient.Mode);
			for (double value : mixed)
				if (!std::isfinite(value)) return (failure = "gradient colour blend is not finite", false);
			uint8_t alpha = SourceGradientByte(a.Alpha + (b.Alpha - a.Alpha) * amount);
			// The pinned Oklab and CMYK helpers multiply an already-byte alpha by 255.
			if (gradient.Mode == 3 || gradient.Mode == 6) alpha = static_cast<uint8_t>(uint32_t(alpha) * 255);
			output = {
				SourceGradientByte(mixed[0] * 255),
				SourceGradientByte(mixed[1] * 255),
				SourceGradientByte(mixed[2] * 255),
				alpha
			};
			return true;
		}
		output = keys.back().Color;
		return true;
	}

	inline Status
	SourceGradientLerpCount(const Gradient &from, const Gradient &to, double amount, size_t &count) {
		if (from.Mode > 6 || to.Mode > 6 || !std::isfinite(amount)) return Status::InvalidValue;
		if (from.Keys.size() > Limits::MaximumGradientKeys || to.Keys.size() > Limits::MaximumGradientKeys)
			return Status::LimitExceeded;
		for (const Gradient *gradient : {&from, &to})
			for (const auto &key : gradient->Keys)
				if (!std::isfinite(key.Time)) return Status::InvalidValue;
		const double keys = std::ceil(
			double(from.Keys.size()) + (double(to.Keys.size()) - double(from.Keys.size())) * amount
		);
		if (!std::isfinite(keys)) return Status::InvalidValue;
		if (keys > Limits::MaximumGradientKeys) return Status::LimitExceeded;
		const size_t candidate = keys <= 0 ? 0 : static_cast<size_t>(keys);
		// The source indexes both arrays for a multi-key result, even when one is
		// empty.
		if (candidate > 1 && (from.Keys.empty() || to.Keys.empty())) return Status::UnsupportedExecution;
		count = candidate;
		return Status::Ok;
	}

	inline Colour SourceGradientMerge(const Colour &a, const Colour &b, double amount) {
		return {
			SourceGradientByte(a.Red + (double(b.Red) - a.Red) * amount),
			SourceGradientByte(a.Green + (double(b.Green) - a.Green) * amount),
			SourceGradientByte(a.Blue + (double(b.Blue) - a.Blue) * amount),
			SourceGradientByte(a.Alpha + (double(b.Alpha) - a.Alpha) * amount)
		};
	}

	// Matches persisted gradientObject.lerpTo, including its left mode and
	// ratio-derived key count.
	inline Status LerpSourceGradient(
		const Gradient &from,
		const Gradient &to,
		double amount,
		Gradient &output,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	) {
		ENGINE_PROFILE("imagegraph.gradient_lerp");
		size_t count = 0;
		const Status admitted = SourceGradientLerpCount(from, to, amount, count);
		if (admitted != Status::Ok) return admitted;
		const uint64_t previousKeys = output.Keys.capacity();
		if (previousKeys > maximumBytes / sizeof(GradientKey) ||
			count > maximumBytes / sizeof(GradientKey) - previousKeys)
			return Status::LimitExceeded;
		Gradient candidate;
		candidate.Mode = from.Mode;
		candidate.Keys.reserve(count);
		std::string_view failure;
		for (size_t index = 0; index < count; index++) {
			double time = 0;
			if (count > 1) {
				const double ratio = double(index) / double(count - 1);
				const double first =
					from.Keys[static_cast<size_t>(ratio * double(from.Keys.size() - 1))].Time;
				const double last = to.Keys[static_cast<size_t>(ratio * double(to.Keys.size() - 1))].Time;
				time = first + (last - first) * amount;
				if (!std::isfinite(time)) return Status::InvalidValue;
			}
			Colour first{}, last{};
			if (!SampleSourceGradient(from, time, first, failure) ||
				!SampleSourceGradient(to, time, last, failure))
				return Status::InvalidValue;
			for (const double value :
				 {first.Red + (double(last.Red) - first.Red) * amount,
				  first.Green + (double(last.Green) - first.Green) * amount,
				  first.Blue + (double(last.Blue) - first.Blue) * amount,
				  first.Alpha + (double(last.Alpha) - first.Alpha) * amount})
				if (!std::isfinite(value)) return Status::InvalidValue;
			candidate.Keys.push_back({time, SourceGradientMerge(first, last, amount)});
		}
		core::Metrics::Count("imagegraph.gradient_lerp.keys", count);
		core::Metrics::Count("imagegraph.gradient_lerp.samples", 2 * count);
		core::Metrics::Count(
			"imagegraph.gradient_lerp.owned_payload_bytes", candidate.Keys.capacity() * sizeof(GradientKey)
		);
		output = std::move(candidate);
		return Status::Ok;
	}
} // namespace engine::imagegraph::detail
