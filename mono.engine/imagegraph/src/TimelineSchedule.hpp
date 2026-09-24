#pragma once

// Pure frame selection. Stateful image history remains with a future sequence evaluator.

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace engine::imagegraph::detail {
	enum class PlaybackEnd : uint8_t { Loop, Stop, PingPong };
	enum class KeyEnd : uint8_t { Hold, Loop, Ping, Wrap };
	enum class CurveSide : uint8_t { Linear, Bezier, Cut };

	struct FrameWindow {
		uint64_t Total = 0;
		uint64_t First = 0;
		uint64_t Last = 0;
	};

	struct PlaybackCursor {
		uint64_t Frame = 0;
		int8_t Direction = 1;
		bool Playing = false;
		bool Rendering = false;
	};

	struct PlaybackStep {
		PlaybackCursor Cursor;
		bool Restart = false;
	};

	struct PlaybackClock {
		double PendingSeconds = 0;
	};

	inline bool ValidFrameWindow(const FrameWindow &window) {
		return window.Total > 0 && window.Total <= Limits::MaximumTick + 1 && window.First <= window.Last &&
			   window.Last < window.Total;
	}

	inline bool BeginPlayback(const FrameWindow &window, bool rendering, PlaybackStep &result) {
		if (!ValidFrameWindow(window)) return false;
		result = {{window.First, 1, true, rendering}, !rendering};
		return true;
	}

	inline bool StopPlayback(const FrameWindow &window, PlaybackStep &result) {
		if (!ValidFrameWindow(window)) return false;
		result = {{window.First, 1, false, false}, false};
		return true;
	}

	// Advances one integer frame. Range-start pingpong returns at frame zero in the pinned source.
	inline bool
	AdvancePlayback(const FrameWindow &window, PlaybackEnd end, PlaybackCursor cursor, PlaybackStep &result) {
		if (!ValidFrameWindow(window) || cursor.Frame >= window.Total ||
			(cursor.Direction != 1 && cursor.Direction != -1))
			return false;
		result = {cursor, false};
		if (!cursor.Playing) return true;
		if (cursor.Direction == 1) {
			if (cursor.Frame < window.Last) {
				result.Cursor.Frame++;
			} else if (end == PlaybackEnd::Stop || cursor.Rendering) {
				result.Cursor.Frame = window.Last;
				result.Cursor.Playing = false;
				result.Cursor.Rendering = false;
			} else if (end == PlaybackEnd::PingPong) {
				result.Cursor.Frame = window.Last == 0 ? 0 : window.Last - 1;
				result.Cursor.Direction = -1;
			} else {
				result.Cursor.Frame = window.First;
				result.Restart = true;
			}
		} else if (cursor.Frame <= 1) {
			result.Cursor.Frame = 0;
			result.Cursor.Direction = 1;
		} else {
			result.Cursor.Frame--;
		}
		return true;
	}

	// The pinned animator consumes at most one frame per call and carries the remaining time forward.
	inline bool AdvancePlaybackTime(
		const FrameWindow &window,
		PlaybackEnd end,
		PlaybackCursor cursor,
		PlaybackClock clock,
		double deltaSeconds,
		double framesPerSecond,
		double speed,
		PlaybackStep &result,
		PlaybackClock &nextClock
	) {
		if (!ValidFrameWindow(window) || !std::isfinite(clock.PendingSeconds) || clock.PendingSeconds < 0 ||
			!std::isfinite(deltaSeconds) || deltaSeconds < 0 || !std::isfinite(framesPerSecond) ||
			framesPerSecond <= 0 || !std::isfinite(speed) || speed < 0 || cursor.Frame >= window.Total ||
			(cursor.Direction != 1 && cursor.Direction != -1))
			return false;
		result = {cursor, false};
		nextClock = clock;
		if (!cursor.Playing) return true;
		const double pending = clock.PendingSeconds + deltaSeconds * speed;
		if (!std::isfinite(pending)) return false;
		const double frameSeconds = 1.0 / framesPerSecond;
		if (pending < frameSeconds) {
			nextClock.PendingSeconds = pending;
			return true;
		}
		if (!AdvancePlayback(window, end, cursor, result)) return false;
		nextClock.PendingSeconds = result.Cursor.Playing ? pending - frameSeconds : 0;
		return true;
	}

	// GameMaker round uses ties to even. Fractional output needs another value type.
	inline bool SeekFrame(double realFrame, bool round, uint64_t &frame) {
		if (!std::isfinite(realFrame) || realFrame < 0 ||
			realFrame > static_cast<double>(Limits::MaximumTick))
			return false;
		const double lower = std::floor(realFrame);
		const double fraction = realFrame - lower;
		if (!round && fraction != 0) return false;
		const double selected =
			round && (fraction > 0.5 || (fraction == 0.5 && static_cast<uint64_t>(lower) % 2 != 0))
				? lower + 1
				: lower;
		frame = static_cast<uint64_t>(selected);
		return true;
	}

	struct KeySelection {
		size_t From = 0;
		size_t To = 0;
		uint64_t Numerator = 0;
		uint64_t Denominator = 1;
	};

	struct FractionalKeySelection {
		size_t From = 0;
		size_t To = 0;
		double Ratio = 0;
	};

	struct KeyEase {
		CurveSide OutType = CurveSide::Linear;
		CurveSide InType = CurveSide::Linear;
		double OutX = 0;
		double OutY = 0;
		double InX = 0;
		double InY = 1;
	};

	enum class KeyChoice : uint8_t { From, To, Blend };

	struct KeyBlend {
		KeyChoice Choice = KeyChoice::Blend;
		double Ratio = 0;
	};

	inline double Cubic(double firstHandle, double secondHandle, double t) {
		const double inverse = 1 - t;
		return 3 * inverse * inverse * t * firstHandle + 3 * inverse * t * t * secondHandle + t * t * t;
	}

	// The source checks the incoming cut before the outgoing cut, then inverts Bezier x for eight steps.
	inline bool EaseKeys(const KeyEase &ease, double ratio, KeyBlend &blend) {
		if (!std::isfinite(ratio) || ratio < 0 || ratio > 1 || !std::isfinite(ease.OutX) ||
			!std::isfinite(ease.OutY) || !std::isfinite(ease.InX) || !std::isfinite(ease.InY))
			return false;
		if (ease.InType == CurveSide::Cut) {
			blend = {KeyChoice::From, 0};
			return true;
		}
		if (ease.OutType == CurveSide::Cut) {
			blend = {KeyChoice::To, 1};
			return true;
		}
		if ((ease.OutType == CurveSide::Linear && ease.InType == CurveSide::Linear) || ratio == 0 ||
			ratio == 1) {
			blend = {KeyChoice::Blend, ratio};
			return true;
		}
		double outX = ease.OutX;
		double outY = ease.OutY;
		double inX = ease.InX;
		double inY = ease.InY;
		if (outX > 1 && inX > 1) {
			const double total = std::min(outX, inX);
			outX /= total;
			inX /= total;
		}
		inY -= std::clamp(outX - 1, 0.0, 1.0);
		outX = std::clamp(outX, 0.0, 0.9);
		outY += std::clamp(inX - 1, 0.0, 1.0);
		inX = std::clamp(inX, 0.0, 0.9);
		const double secondX = 1 - inX;
		double t = ratio;
		for (uint8_t step = 0; step < 8; step++) {
			const double inverse = 1 - t;
			const double x = Cubic(outX, secondX, t);
			if (std::abs(x - ratio) < 0.0001) break;
			const double derivative =
				3 * inverse * inverse * outX + 6 * inverse * t * (secondX - outX) + 3 * t * t * (1 - secondX);
			if (derivative == 0 || !std::isfinite(derivative)) return false;
			t -= (x - ratio) / derivative;
			if (!std::isfinite(t)) return false;
		}
		const double eased = Cubic(outY, inY, t);
		if (!std::isfinite(eased)) return false;
		blend = {KeyChoice::Blend, eased};
		return true;
	}

	inline Status InterpolateEased(const Value &from, const Value &to, double ratio, Value &result) {
		if (!std::isfinite(ratio)) return Status::InvalidValue;
		if (from.index() != to.index()) return Status::TypeMismatch;
		if (const auto *scalar = std::get_if<double>(&from)) {
			const double value = std::lerp(*scalar, std::get<double>(to), ratio);
			if (!std::isfinite(value)) return Status::InvalidValue;
			result = value;
			return Status::Ok;
		}
		if (const auto *vector = std::get_if<Vector2>(&from)) {
			const Vector2 end = std::get<Vector2>(to);
			const Vector2 value{std::lerp(vector->X, end.X, ratio), std::lerp(vector->Y, end.Y, ratio)};
			if (!std::isfinite(value.X) || !std::isfinite(value.Y)) return Status::InvalidValue;
			result = value;
			return Status::Ok;
		}
		if (const auto *vector = std::get_if<Vector4>(&from)) {
			const Vector4 end = std::get<Vector4>(to);
			const Vector4 value{
				std::lerp(vector->X, end.X, ratio),
				std::lerp(vector->Y, end.Y, ratio),
				std::lerp(vector->Z, end.Z, ratio),
				std::lerp(vector->W, end.W, ratio),
			};
			if (!std::isfinite(value.X) || !std::isfinite(value.Y) || !std::isfinite(value.Z) ||
				!std::isfinite(value.W))
				return Status::InvalidValue;
			result = value;
			return Status::Ok;
		}
		if (const auto *colour = std::get_if<Colour>(&from)) {
			const Colour end = std::get<Colour>(to);
			const auto channel = [ratio](uint8_t first, uint8_t last) {
				return static_cast<uint8_t>(
					std::lround(std::clamp(std::lerp(double(first), double(last), ratio), 0.0, 255.0))
				);
			};
			result = Colour{
				channel(colour->Red, end.Red),
				channel(colour->Green, end.Green),
				channel(colour->Blue, end.Blue),
				channel(colour->Alpha, end.Alpha),
			};
			return Status::Ok;
		}
		return Status::UnsupportedExecution;
	}

	// Key ticks must be strictly increasing. LoopRangeStart selects the first key of the loop tail.
	inline bool SelectKeys(
		std::span<const uint64_t> ticks,
		uint64_t tick,
		uint64_t totalFrames,
		KeyEnd end,
		size_t loopRangeStart,
		KeySelection &selection
	) {
		if (ticks.empty() || ticks.size() > Limits::MaximumKeyframes || tick > Limits::MaximumTick ||
			totalFrames == 0 || totalFrames > Limits::MaximumTick + 1 || loopRangeStart >= ticks.size())
			return false;
		for (size_t index = 0; index < ticks.size(); index++) {
			if (ticks[index] > Limits::MaximumTick || (index && ticks[index - 1] >= ticks[index]))
				return false;
		}
		const size_t lastIndex = ticks.size() - 1;
		const uint64_t first = ticks[loopRangeStart];
		const uint64_t last = ticks[lastIndex];
		if (end == KeyEnd::Wrap &&
			(ticks.front() >= totalFrames || last >= totalFrames || tick >= totalFrames))
			return false;
		if (ticks.size() == 1) {
			selection = {0, 0, 0, 1};
			return true;
		}
		if (tick > last && end == KeyEnd::Loop) {
			const uint64_t period = last - first + 1;
			tick = first + (tick - last) % period;
		} else if (tick > last && end == KeyEnd::Ping) {
			const uint64_t duration = last - first;
			if (duration == 0) {
				tick = first;
			} else {
				const uint64_t phase = (tick - first) % (duration * 2);
				tick = phase < duration ? first + phase : first + duration * 2 - phase;
			}
		}
		// The source classifies frame zero as before-first even when the first key is at zero.
		if (tick < ticks.front() || (end == KeyEnd::Wrap && tick == 0)) {
			if (end == KeyEnd::Wrap) {
				selection = {lastIndex, 0, totalFrames - last + tick, totalFrames - last + ticks.front()};
			} else {
				selection = {0, 0, 0, 1};
			}
			return true;
		}
		if (tick >= last) {
			if (end == KeyEnd::Wrap) {
				selection = {lastIndex, 0, tick - last, totalFrames - last + ticks.front()};
			} else {
				selection = {lastIndex, lastIndex, 0, 1};
			}
			return true;
		}
		for (size_t index = 1; index < ticks.size(); index++) {
			if (tick < ticks[index]) {
				selection = {index - 1, index, tick - ticks[index - 1], ticks[index] - ticks[index - 1]};
				return true;
			}
		}
		return false;
	}

	// Fractional time follows the source's continuous loop, ping and wrap arithmetic.
	// Integer requests use SelectKeys so legacy tick results remain exact.
	inline bool SelectKeysFractional(
		std::span<const uint64_t> ticks,
		uint64_t tick,
		double subframe,
		uint64_t totalFrames,
		KeyEnd end,
		size_t loopRangeStart,
		FractionalKeySelection &selection
	) {
		if (ticks.empty() || ticks.size() > Limits::MaximumKeyframes || tick >= Limits::MaximumTick ||
			!std::isfinite(subframe) || subframe <= 0 || subframe >= 1 || totalFrames == 0 ||
			totalFrames > Limits::MaximumTick + 1 || loopRangeStart >= ticks.size())
			return false;
		for (size_t index = 0; index < ticks.size(); index++) {
			if (ticks[index] > Limits::MaximumTick || (index && ticks[index - 1] >= ticks[index]))
				return false;
		}
		const size_t lastIndex = ticks.size() - 1;
		const uint64_t first = ticks[loopRangeStart];
		const uint64_t last = ticks[lastIndex];
		if (end == KeyEnd::Wrap &&
			(ticks.front() >= totalFrames || last >= totalFrames || tick >= totalFrames))
			return false;
		if (ticks.size() == 1) {
			selection = {0, 0, 0};
			return true;
		}
		long double sample = static_cast<long double>(tick) + static_cast<long double>(subframe);
		if (sample > last && end == KeyEnd::Loop) {
			const uint64_t period = last - first + 1;
			sample = first + std::fmod(sample - last, static_cast<long double>(period));
		} else if (sample > last && end == KeyEnd::Ping) {
			const uint64_t duration = last - first;
			if (duration == 0) {
				sample = first;
			} else {
				const long double phase = std::fmod(sample - first, static_cast<long double>(duration) * 2);
				sample = phase < duration ? first + phase : first + duration * 2 - phase;
			}
		}
		if (sample < ticks.front()) {
			if (end == KeyEnd::Wrap) {
				const long double span = static_cast<long double>(totalFrames - last + ticks.front());
				selection = {lastIndex, 0, static_cast<double>((totalFrames - last + sample) / span)};
			} else {
				selection = {0, 0, 0};
			}
			return true;
		}
		if (sample >= last) {
			if (end == KeyEnd::Wrap) {
				const long double span = static_cast<long double>(totalFrames - last + ticks.front());
				selection = {lastIndex, 0, static_cast<double>((sample - last) / span)};
			} else {
				selection = {lastIndex, lastIndex, 0};
			}
			return true;
		}
		for (size_t index = 1; index < ticks.size(); index++) {
			if (sample < ticks[index]) {
				selection = {
					index - 1,
					index,
					static_cast<double>((sample - ticks[index - 1]) / (ticks[index] - ticks[index - 1]))
				};
				return true;
			}
		}
		return false;
	}
}
