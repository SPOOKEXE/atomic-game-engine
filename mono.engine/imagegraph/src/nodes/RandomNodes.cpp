#include "../SourceRandom.hpp"
#include "../TimelineDrivers.hpp"
#include "Curve.hpp"
#include "Families.hpp"

#include <engine/imagegraph/RandomReplay.hpp>
#include <engine/imagegraph/SourceTimeline.hpp>

#include <array>
#include <cmath>
#include <numbers>

namespace engine::imagegraph::detail {
	namespace {
		double Gaussian(SourceRandom &random, double mean, double variance) {
			const double first = random.Unit(), second = random.Unit();
			return mean +
				   std::sqrt(-2 * std::log(first)) * std::cos(2 * std::numbers::pi * second) * variance;
		}
		bool RandomNode(NodeContext &context) {
			const auto &request = context.Request;
			if (request.NegativeFrame)
				return context.Fail(
					Status::InvalidValue, "source random history requires a nonnegative frame"
				);
			const double frame = double(request.Tick) + request.Subframe;
			const auto *replay = context.CurrentRandom ? context.CurrentRandom : request.RandomReplay;
			if (replay) {
				Diagnostic diagnostic;
				const Status status = ValidateRandomReplay(*replay, context.ByteBudget, diagnostic);
				if (status != Status::Ok) return context.Fail(status, diagnostic.Message);
			}

			const RandomReplayEntry *previous = nullptr;
			if (replay)
				for (const auto &entry : replay->Entries)
					if (entry.NodeId == context.Authored.Id && entry.ProcessorRow == context.ProcessorRow)
						previous = &entry;
			const bool shuffle = context.Boolean("shuffle");
			const double smoothing = context.SourceChoice("smoothing");
			if (previous && (previous->Tick > request.Tick ||
							 (previous->Tick == request.Tick && previous->Subframe > request.Subframe)))
				return context.Fail(Status::InvalidValue, "random replay frame moves backwards");
			if (!previous && request.Tick > 0 && (shuffle || smoothing != 0))
				return context.Fail(
					Status::UnsupportedExecution, "random node requires preceding row replay state"
				);
			uint64_t bytes = previous ? RetainedRandomEntryBytes(*previous)
									  : sizeof(RandomReplayEntry) + context.Authored.Id.capacity();
			const uint64_t frames = context.Timeline ? context.Timeline->Frames : 1;
			if (smoothing == 2) {
				if (frames > Limits::MaximumArrayElements)
					return context.Fail(
						Status::LimitExceeded,
						"random convolution timeline exceeds frame budget",
						"kernel_span"
					);
				bytes += frames * sizeof(double);
			}
			if (!context.ReserveOutput(bytes, "result")) return false;
			RandomReplayEntry state = previous ? *previous : RandomReplayEntry{};
			state.NodeId = context.Authored.Id;
			state.ProcessorRow = context.ProcessorRow;
			const RandomEntropyCapture *entropy = nullptr;
			for (const auto &capture : request.RandomEntropy)
				if (capture.NodeId == state.NodeId && capture.ProcessorRow == state.ProcessorRow &&
					capture.Tick == request.Tick && capture.Subframe == request.Subframe) {
					if (entropy)
						return context.Fail(Status::DuplicateId, "random entropy repeats processor identity");
					entropy = &capture;
				}
			const bool deterministic = context.Boolean("deterministic", true);
			if (frame == 0 || !shuffle) {
				uint32_t seed = uint32_t(context.Integer("seed"));
				if (!deterministic) {
					if (!entropy)
						return context.Fail(
							Status::UnsupportedExecution,
							"source current_time entropy recording is unavailable",
							"deterministic"
						);
					seed += uint32_t(entropy->CurrentTimeMilliseconds % 100000);
				}
				state.StoredSeed = seed;
				state.Accumulation = 0;
				state.MovingAverage = 0;
			}
			if (shuffle) {
				const double mode = context.SourceChoice("mode");
				bool reshuffle = false;
				if (mode == 0)
					reshuffle = true;
				else if (mode == 1) {
					const int64_t period = context.Integer("period", 1);
					if (!period)
						return context.Fail(Status::InvalidValue, "random period must be nonzero", "period");
					reshuffle =
						std::fmod(frame - double(context.Integer("period_shift")), double(period)) == 0;
				} else if (mode == 2)
					reshuffle = context.Boolean("trigger");
				else if (mode == 3) {
					SourceRandom random(state.StoredSeed + uint32_t(request.Tick));
					reshuffle = random.Unit() <= context.Scalar("probability", 1);
				} else if (mode == 4) {
					SourceRandom random(state.StoredSeed + uint32_t(request.Tick));
					const double period = Gaussian(
						random, context.Scalar("average_period", 4), context.Scalar("period_variance", 2)
					);
					if (period > 0) state.Accumulation += 1 / period;
					reshuffle = state.Accumulation > 1;
					state.Accumulation -= std::trunc(state.Accumulation);
				} else if (mode == 5) {
					const auto first = context.Timeline ? SourceTimelineFirstFrame(*context.Timeline)
														: std::optional<double>{0.};
					if (!first)
						return context.Fail(
							Status::InvalidValue, "source first-frame bound is invalid", "shuffle"
						);
					reshuffle = frame == *first;
				}
				if (reshuffle) {
					if (!entropy)
						return context.Fail(
							Status::UnsupportedExecution,
							"source randomize seed recording is unavailable",
							"shuffle"
						);
					if (entropy->ReshuffleSeed < 100000 || entropy->ReshuffleSeed > 999999)
						return context.Fail(
							Status::InvalidValue,
							"source seed_random recording must contain six digits",
							"shuffle"
						);
					state.StoredSeed = entropy->ReshuffleSeed;
				}
			}
			SourceRandom random(state.StoredSeed);
			const double distribution = context.SourceChoice("distribution");
			double value = 0;
			if (distribution == 0) {
				const double from = context.Scalar("from"), to = context.Scalar("to", 1);
				value = from == to ? from : std::min(from, to) + random.Unit() * std::abs(to - from);
			} else if (distribution == 1)
				value = Gaussian(random, context.Scalar("mean"), context.Scalar("variance", 1));
			else if (distribution == 2)
				value = random.Unit() <= context.Scalar("p", .5);
			else if (distribution == 3) {
				const double trials = context.Scalar("t", 1);
				if (!std::isfinite(trials) || trials > Limits::MaximumArrayElements)
					return context.Fail(
						Status::LimitExceeded, "binomial trial count exceeds work budget", "t"
					);
				const int64_t count = int64_t(std::max(0.0, DriverRoundHalfEven(trials)));
				for (int64_t index = 0; index < count; ++index)
					value += random.Unit() <= context.Scalar("p", .5);
			} else if (distribution == 4) {
				const auto *curveValue = context.Find("dist_curve");
				const auto *curve = curveValue ? std::get_if<Curve>(curveValue) : nullptr;
				if (!curve)
					return context.Fail(
						Status::InvalidValue, "custom random distribution requires a curve", "dist_curve"
					);
				std::array<double, 128> cmf{};
				double total = 0;
				for (size_t index = 0; index < cmf.size(); ++index) {
					cmf[index] = total;
					total += EvalCurveX(*curve, double(index) / 127);
				}
				if (!std::isfinite(total) || total == 0)
					return context.Fail(
						Status::InvalidValue,
						"custom distribution integral is zero or nonfinite",
						"dist_curve"
					);
				for (double &entry : cmf)
					entry /= total;
				double low = 0, high = 127;
				const double chosen = random.Unit();
				for (size_t iteration = 0; high - low > 1 && iteration < 128; ++iteration) {
					if (cmf[size_t(low)] == chosen || cmf[size_t(high)] == chosen) break;
					const double middle = DriverRoundHalfEven(low + high) / 2;
					if (cmf[size_t(middle)] > chosen)
						high = middle;
					else
						low = middle;
				}
				const double denominator = cmf[size_t(high)] - cmf[size_t(low)];
				if (denominator == 0)
					return context.Fail(
						Status::InvalidValue, "custom distribution bracket has zero probability", "dist_curve"
					);
				const double at = CurveLerp(low, high, (chosen - cmf[size_t(low)]) / denominator);
				value = CurveLerp(context.Scalar("from"), context.Scalar("to", 1), at / 127);
			} else
				return context.Fail(
					Status::InvalidValue,
					"source random distribution produces undefined value",
					"distribution"
				);
			double output = value;
			if (smoothing == 1) {
				const int64_t window = context.Integer("window_size", 5);
				if (window < 1)
					return context.Fail(
						Status::InvalidValue, "random moving average requires positive window", "window_size"
					);
				state.MovingAverage =
					(state.MovingAverage * std::clamp(frame, 0.0, double(window - 1)) + value) /
					std::clamp(frame + 1, 1.0, double(window));
				output = state.MovingAverage;
			} else if (smoothing == 2) {
				if (request.Tick >= frames)
					return context.Fail(
						Status::InvalidValue, "random convolution frame is outside timeline", "kernel"
					);
				state.Kernel.resize(size_t(frames), 0);
				state.Kernel[size_t(request.Tick)] = value;
				const int64_t span = context.Integer("kernel_span", 3);
				if (span > int64_t(Limits::MaximumArrayElements))
					return context.Fail(
						Status::LimitExceeded, "random kernel span exceeds work budget", "kernel_span"
					);
				if (span > 0) {
					const Value *raw = context.Find("kernel");
					const auto *curve = raw ? std::get_if<Curve>(raw) : nullptr;
					if (!curve)
						return context.Fail(
							Status::InvalidValue, "random convolution requires a kernel curve", "kernel"
						);
					double sum = 0, weights = 0;
					for (int64_t offset = -span; offset <= span; ++offset) {
						const int64_t index = int64_t(request.Tick) + offset;
						if (index < 0 || uint64_t(index) >= frames) continue;
						const double weight = EvalCurveX(*curve, double(std::abs(offset)) / double(span));
						sum += weight * state.Kernel[size_t(index)];
						weights += weight;
					}
					output = sum / weights;
				}
			} else if (smoothing == 3)
				output = CurveLerp(state.PreviousOutput, value, context.Scalar("lerp_ratio", .5));
			if (context.Boolean("integer")) output = DriverRoundHalfEven(output);
			if (!std::isfinite(output) || !std::isfinite(state.Accumulation) ||
				!std::isfinite(state.MovingAverage))
				return context.Fail(
					Status::InvalidValue, "source random result or history is nonfinite", "result"
				);
			state.PreviousOutput = output;
			state.Tick = request.Tick;
			state.Subframe = request.Subframe;
			state.Initialized = true;
			context.RandomUpdates.push_back(std::move(state));
			context.SetValue("result", output);
			return context.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> RandomExecutors() {
		static constexpr std::array entries{ExecutorEntry{"pc.random", RandomNode}};
		return entries;
	}
}
