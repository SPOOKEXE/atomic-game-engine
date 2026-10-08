#pragma once

#include "SourcePathSequentialMath.hpp"
#include "SourcePathShape.hpp"
#include "SourcePathShiftMemo.hpp"
#include "SourcePathWaveRandom.hpp"
#include "SourcePathWeight.hpp"

namespace engine::imagegraph::detail {
	inline double SourceWaveLengthMultiplier(const SourcePathWaveData2D &controls) {
		const double frequency = std::max(1., std::abs(std::max(controls.Frequency.X, controls.Frequency.Y)));
		const double amplitude = std::max(controls.Amplitude.X, controls.Amplitude.Y);
		return frequency * std::sqrt(std::abs(amplitude) + 1 / frequency);
	}
	// A miss samples the child in source order; only a complete sample commits buffers and its key.
	template <class Sample>
	SourcePathPointBuffer SampleSourceWavePath(
		NodeContext &context,
		const SourcePathData2D &operation,
		double ratio,
		size_t line,
		SourcePathPointBuffer &out,
		const Sample &sample
	) {
		ENGINE_PROFILE("imagegraph.source.path_wave.sample");
		out.Position = {};
		if (!context.PathShiftMemo) {
			context.Fail(Status::UnsupportedExecution, "Source Wave requires evaluation-owned memo", "path");
			return out;
		}
		auto &memo = *context.PathShiftMemo;
		const auto &controls = *operation.Wave;
		if (line >= Limits::MaximumArrayElements) {
			context.Fail(
				Status::LimitExceeded, "Source Wave line identity exceeds bounded cache slots", "path"
			);
			return out;
		}
		if (controls.Iteration < 0 || controls.Iteration > int64_t(Limits::MaximumArrayElements)) {
			context.Fail(
				Status::UnsupportedExecution,
				"Source Wave iteration count is outside the bounded HTML5 profile",
				"iteration"
			);
			return out;
		}
		const auto key = SourceShiftRatioKey(ratio);
		if (!key) {
			context.Fail(Status::InvalidValue, "Source Wave ratio cache key is undefined", "path");
			return out;
		}
		auto buffers = controls.Buffers;
		if (!memo.ValidationProbe) {
			auto *owner = memo.WaveOwner(context, operation);
			if (!owner) return out;
			buffers = owner->WaveBuffers;
			if (const auto *hit = memo.Find(context, operation.EvaluationMemoId, *key, line)) {
				core::Metrics::Count("imagegraph.path.wave.cache_hits", 1);
				out.Position = {hit->Point.X, hit->Point.Y};
				out.Weight = hit->Point.Weight;
				return out;
			}
		}
		if (context.FailureCode != Status::Ok) return out;
		core::Metrics::Count("imagegraph.path.wave.cache_misses", 1);
		const double original = ratio;
		const auto child = [&](double position, size_t index) {
			if (!memo.Step(context)) return false;
			core::Metrics::Count("imagegraph.path.wave.child_samples", 1);
			// Child methods may return a different point class, including its independent Z field.
			buffers[index] = sample(position, line, buffers[index]);
			return context.FailureCode == Status::Ok && SourceSequentialFinitePoint(buffers[index]);
		};
		const auto fail = [&]() {
			context.Fail(
				Status::InvalidValue, "Source Wave sample geometry or arithmetic is undefined", "path"
			);
			return out;
		};
		if (ratio < controls.Range.X || ratio > controls.Range.Y) {
			if (!child(ratio, 0)) return fail();
			out.Position = buffers[0].Position;
			out.Weight = buffers[0].Weight;
		} else {
			const auto wrapped = [](double position) -> std::optional<double> {
				const auto first = SourcePathWaveFrac(position * .9999);
				return first ? SourcePathWaveFrac(*first + 1) : std::nullopt;
			};
			const auto position = [&](double offset) -> std::optional<double> {
				return controls.Loop ? wrapped(ratio + offset)
									 : std::optional{std::clamp(ratio + offset, 0., .99)};
			};
			if (!controls.Loop) ratio = std::clamp(ratio, 0., .99);
			double direction = 0;
			if (controls.Direction == 0) {
				const auto before = position(-.01), center = position(0), after = position(.01);
				if (!before || !center || !after || !child(*before, 1) || !child(*center, 0) ||
					!child(*after, 2))
					return fail();
				direction = SourceWeightDirection(
								buffers[2].Position.X - buffers[1].Position.X,
								buffers[2].Position.Y - buffers[1].Position.Y
							) +
							90;
			} else {
				const auto center = position(0);
				if (!center || !child(*center, 0)) return fail();
				const double blend = controls.DirectionCurve.empty()
										 ? ratio
										 : SourceWeightCurve(controls.DirectionCurve, ratio);
				direction = controls.DirectionRange.X +
							(controls.DirectionRange.Y - controls.DirectionRange.X) * blend;
			}
			SourcePathWaveRandom random;
			const double seed = controls.Seed + double(line);
			const auto amplitudeDraw =
				random.SeededRange(controls.Amplitude.X, controls.Amplitude.Y, seed + double(line));
			const auto phaseDraw =
				random.SeededRange(controls.Phase.X, controls.Phase.Y, seed + double(line) + 1);
			const auto frequencyDraw =
				random.SeededRange(controls.Frequency.X, controls.Frequency.Y, seed + double(line) + 2);
			if (!amplitudeDraw || !phaseDraw || !frequencyDraw) return fail();
			double amplitude = *amplitudeDraw, phase = *phaseDraw;
			double frequency = std::max(.01, std::abs(*frequencyDraw));
			if (controls.Wiggle) {
				const auto time = SourcePathWaveFrac(phase + ratio * frequency);
				if (!time) return fail();
				const auto wiggle = random.Wiggle(
					controls.WiggleAmplitude.X,
					controls.WiggleAmplitude.Y,
					controls.WiggleFrequency,
					*time,
					seed
				);
				if (!wiggle) return fail();
				amplitude += *wiggle;
			}
			double progress = 0, scale = 1;
			for (int64_t iteration = 0; iteration < controls.Iteration; ++iteration) {
				if (!memo.Step(context)) return out;
				core::Metrics::Count("imagegraph.path.wave.iterations", 1);
				const double time = phase + ratio * frequency;
				if (!std::isfinite(time)) return fail();
				if (controls.Mode == 1)
					progress += scale * std::cos(time * std::numbers::pi * 2);
				else {
					const auto fraction = SourcePathWaveFrac(time);
					if (!fraction) return fail();
					progress += scale * (controls.Mode == 0 ? (std::abs(*fraction * 2 - 1) - .5) * 2
															: double(*fraction > .5) * 2 - 1);
				}
				frequency += controls.IterationFrequency;
				scale *= controls.IterationAmplitude;
				// The final iteration consumes its draw too; this is the source call sequence.
				phase += controls.IterationShift + random.Unit();
				if (!std::isfinite(progress) || !std::isfinite(frequency) || !std::isfinite(scale) ||
					!std::isfinite(phase))
					return fail();
			}
			if (controls.Post == 1)
				progress = std::abs(progress);
			else if (controls.Post == 2)
				progress = std::max(progress, 0.);
			const double curveRatio = controls.ClampCurve
										  ? (ratio - controls.Range.X) / (controls.Range.Y - controls.Range.X)
										  : ratio;
			const auto fraction = SourcePathWaveFrac(curveRatio + controls.AmplitudeShift);
			const auto curvePosition = fraction ? SourcePathWaveFrac(*fraction + 1) : std::nullopt;
			if (!curvePosition || controls.AmplitudeCurve.empty()) return fail();
			progress *= SourceWeightCurve(controls.AmplitudeCurve, *curvePosition);
			const double radians = direction * std::numbers::pi / 180;
			const double displacement = amplitude * progress;
			out.Position = {
				buffers[0].Position.X + SourceShapeLengthdirComponent(displacement * std::cos(radians)),
				buffers[0].Position.Y + SourceShapeLengthdirComponent(-displacement * std::sin(radians))
			};
			out.Weight = buffers[0].Weight;
			if (controls.UseWeight) {
				const double weight =
					controls.WeightRange.X + (controls.WeightRange.Y - controls.WeightRange.X) * progress;
				if (controls.WeightMode == 0)
					out.Weight = weight;
				else if (controls.WeightMode == 1)
					out.Weight += weight;
				else
					out.Weight *= weight;
			}
		}
		if (!std::isfinite(out.Position.X) || !std::isfinite(out.Position.Y) || !std::isfinite(out.Weight))
			return fail();
		if (!memo.ValidationProbe) {
			if (!memo.Store(
					context,
					operation.EvaluationMemoId,
					*key,
					line,
					{out.Position.X, out.Position.Y, out.Weight},
					original
				))
				return out;
			// Nested children may grow Owners, so never keep an owner pointer across their calls.
			memo.Owners[operation.EvaluationMemoId - 1].WaveBuffers = buffers;
		}
		return out;
	}
}
