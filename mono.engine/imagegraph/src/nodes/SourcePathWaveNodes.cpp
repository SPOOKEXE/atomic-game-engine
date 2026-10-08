#include "SourcePathWaveNodes.hpp"

#include "../SourcePathPayload3D.hpp"
#include "../SourcePathShiftMemo.hpp"
#include "../SourcePathWeight.hpp"
#include "Curve.hpp"

#include <engine/imagegraph/FrameTime.hpp>

#include <cmath>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t WAVE_WORK_LIMIT = uint64_t{1} << 24;
		struct WaveQuote {
			SourcePathWaveData2D Controls;
			const Path2D *Planar = nullptr;
			const PathData3D *Spatial = nullptr;
			const Curve *AmplitudeCurve = nullptr, *DirectionCurve = nullptr;
			uint64_t Bytes = 0, Work = 0;
		};
		bool WaveVector(NodeContext &context, std::string_view port, Vector2 &out) {
			const auto *value = context.Find(port);
			if (value && !std::holds_alternative<Vector2>(*value) &&
				!std::holds_alternative<double>(*value) && !std::holds_alternative<int64_t>(*value))
				return context.Fail(Status::UnsupportedExecution, "Wave range getter is unrepresented", port);
			out = context.Vec2(port, out);
			return MeshFinite(out) || context.Fail(Status::InvalidValue, "Wave range must be finite", port);
		}
		bool WaveScalar(NodeContext &context, std::string_view port, double &out) {
			const auto *value = context.Find(port);
			if (value && !std::holds_alternative<double>(*value) && !std::holds_alternative<int64_t>(*value))
				return context.Fail(
					Status::UnsupportedExecution, "Wave numeric getter is unrepresented", port
				);
			out = context.Scalar(port, out);
			return std::isfinite(out) ||
				   context.Fail(Status::InvalidValue, "Wave control must be finite", port);
		}
		bool WaveChoice(NodeContext &context, std::string_view port, uint8_t maximum, uint8_t &out) {
			const double choice = context.SourceChoice(port);
			if (context.FailureCode != Status::Ok) return false;
			if (!std::isfinite(choice) || choice < 0 || choice > maximum)
				return context.Fail(Status::InvalidValue, "Wave choice is outside its source branches", port);
			if (std::trunc(choice) != choice)
				return context.Fail(
					Status::UnsupportedExecution, "Wave fractional choice has no source branch", port
				);
			out = uint8_t(choice);
			return true;
		}
		bool WaveCurve(
			NodeContext &context,
			std::string_view port,
			uint64_t precision,
			const Curve *&curve,
			uint64_t &work
		) {
			const auto *value = context.Find(port);
			curve = value ? std::get_if<Curve>(value) : nullptr;
			if (!curve)
				return context.Fail(
					Status::UnsupportedExecution, "Wave requires a represented source curve", port
				);
			if (curve->Anchors.size() > Limits::MaximumCurveAnchors)
				return context.Fail(Status::LimitExceeded, "Wave curve anchor count exceeds bounds", port);
			const uint64_t cost = (precision + 1) * std::max<size_t>(1, curve->Anchors.size()) * 264 * 3;
			if (cost > WAVE_WORK_LIMIT - work)
				return context.Fail(Status::LimitExceeded, "Wave curve generation work exceeds bounds", port);
			work += cost;
			// Validate the complete source table without allocating or adding sample history.
			for (uint64_t i = 0; i <= precision; ++i)
				if (!std::isfinite(EvalCurveX(*curve, double(i) / precision, 1e-5)))
					return context.Fail(
						Status::InvalidValue, "Wave curve table contains a nonfinite sample", port
					);
			return true;
		}
		bool WaveDerived(NodeContext &context, const SourcePathWaveData2D &controls) {
			const double frequency =
							 std::max(1., std::abs(std::max(controls.Frequency.X, controls.Frequency.Y))),
						 amplitude = std::abs(std::max(controls.Amplitude.X, controls.Amplitude.Y));
			if (!std::isfinite(frequency * std::sqrt(amplitude + 1 / frequency)))
				return context.Fail(
					Status::InvalidValue, "Wave derived metric scale is nonfinite", "amplitude"
				);
			double coefficient = 1, coefficientSum = 0;
			for (int64_t i = 0; i < controls.Iteration; ++i) {
				coefficientSum += std::abs(coefficient);
				coefficient *= controls.IterationAmplitude;
				if (!std::isfinite(coefficient) || !std::isfinite(coefficientSum))
					return context.Fail(
						Status::InvalidValue, "Wave iteration amplitude is nonfinite", "amplitude_2"
					);
			}
			const double iterations = double(std::max<int64_t>(controls.Iteration, 0));
			const double maxAmplitude =
				std::max(std::abs(controls.Amplitude.X), std::abs(controls.Amplitude.Y)) +
				(controls.Wiggle
					 ? std::max(std::abs(controls.WiggleAmplitude.X), std::abs(controls.WiggleAmplitude.Y))
					 : 0);
			if (!std::isfinite(maxAmplitude * coefficientSum))
				return context.Fail(
					Status::InvalidValue, "Wave displacement envelope is nonfinite", "amplitude"
				);
			if (!std::isfinite(
					std::max(std::abs(controls.Frequency.X), std::abs(controls.Frequency.Y)) +
					iterations * std::abs(controls.IterationFrequency)
				))
				return context.Fail(
					Status::InvalidValue, "Wave iteration frequency is nonfinite", "freqency"
				);
			if (!std::isfinite(
					std::max(std::abs(controls.Phase.X), std::abs(controls.Phase.Y)) +
					iterations * (std::abs(controls.IterationShift) + 1)
				))
				return context.Fail(Status::InvalidValue, "Wave iteration phase is nonfinite", "shift_2");
			return true;
		}
		bool QuoteWave(NodeContext &context, WaveQuote &quote) {
			auto &controls = quote.Controls;
			if (!context.Find("seed"))
				return context.Fail(
					Status::UnsupportedExecution,
					"Wave requires the source constructor seed observation",
					"seed"
				);
			if (!WaveScalar(context, "seed", controls.Seed)) return false;
			controls.Seed += double(context.ProcessorRow);
			if (!std::isfinite(controls.Seed + 2 * double(Limits::MaximumArrayElements) + 3))
				return context.Fail(Status::InvalidValue, "Wave row or line seed is nonfinite", "seed");
			if (!WaveVector(context, "range", controls.Range) ||
				!WaveVector(context, "frequency", controls.Frequency) ||
				!WaveVector(context, "amplitude", controls.Amplitude) ||
				!WaveVector(context, "phase", controls.Phase) ||
				!WaveVector(context, "angle", controls.DirectionRange) ||
				!WaveVector(context, "wiggle_amplitude", controls.WiggleAmplitude) ||
				!WaveVector(context, "range_2", controls.WeightRange) ||
				!WaveScalar(context, "shift", controls.AmplitudeShift) ||
				!WaveScalar(context, "freqency", controls.IterationFrequency) ||
				!WaveScalar(context, "amplitude_2", controls.IterationAmplitude) ||
				!WaveScalar(context, "shift_2", controls.IterationShift) ||
				!WaveScalar(context, "wiggle_frequency", controls.WiggleFrequency) ||
				!WaveChoice(context, "direction", 1, controls.Direction) ||
				!WaveChoice(context, "mode", 2, controls.Mode) ||
				!WaveChoice(context, "post_fn", 2, controls.Post) ||
				!WaveChoice(context, "weight_mode", 2, controls.WeightMode))
				return false;
			controls.ClampCurve = context.Boolean("clamp_curve");
			controls.Loop = context.Boolean("loop");
			controls.Wiggle = context.Boolean("wiggle");
			controls.UseWeight = context.Boolean("use_weight");
			controls.Iteration = context.Integer("iteration", 1);
			if (context.FailureCode != Status::Ok) return false;
			if (controls.Iteration < 0)
				return context.Fail(
					Status::UnsupportedExecution,
					"Wave negative repetition is outside the represented profile",
					"iteration"
				);
			if (controls.Iteration > int64_t(Limits::MaximumArrayElements))
				return context.Fail(
					Status::LimitExceeded, "Wave iteration count exceeds bounds", "iteration"
				);
			if (!WaveDerived(context, controls)) return false;
			quote.Work = uint64_t(std::max<int64_t>(controls.Iteration, 0)) * 32 + 256;
			if (!WaveCurve(context, "amplitude_curve", 128, quote.AmplitudeCurve, quote.Work)) return false;
			if (context.Boolean("angle_curved") &&
				!WaveCurve(context, "angle_curve", 32, quote.DirectionCurve, quote.Work))
				return false;
			const auto provenance = context.IsCatalogueDefault("path");
			if (!provenance)
				return context.Fail(
					Status::UnsupportedExecution, "Wave requires resolved path default provenance", "path"
				);
			const auto *value = context.Find("path");
			const auto *scalar = value ? std::get_if<double>(value) : nullptr;
			const auto *integer = value ? std::get_if<int64_t>(value) : nullptr;
			const bool absent = *provenance || !value || std::holds_alternative<UndefinedValue>(*value) ||
								(scalar && *scalar == -4) || (integer && *integer == -4);
			uint64_t childBytes = 0;
			if (!absent) {
				quote.Planar = std::get_if<Path2D>(value);
				const auto *spatial = std::get_if<PathValue3D>(value);
				quote.Spatial = spatial && spatial->Data ? spatial->Data.operator->() : nullptr;
				if (!quote.Planar && !quote.Spatial)
					return context.Fail(
						Status::UnsupportedExecution, "Wave requires a planar or spatial source path", "path"
					);
				const auto childWork = SourceWeightRuntimeWork(quote.Planar, quote.Spatial);
				const uint64_t repeats = controls.Direction == 0 ? 3 : 1;
				if (!childWork || *childWork > (WAVE_WORK_LIMIT - quote.Work) / repeats)
					return context.Fail(
						Status::LimitExceeded, "Wave child sampling work exceeds bounds", "path"
					);
				quote.Work += *childWork * repeats;
				if ((quote.Planar && !ValidSourcePath2D(*quote.Planar)) ||
					(quote.Spatial && !ValidSourcePath3D(*quote.Spatial)))
					return context.Fail(Status::InvalidValue, "Wave child path payload is invalid", "path");
				childBytes = quote.Planar
								 ? MeshAddBytes(sizeof(Path2D), SourcePath2DBytes<false>(*quote.Planar))
							 : quote.Spatial ? SourcePath3DBytes<false>(*quote.Spatial)
											 : 0;
			}
			const uint64_t tableBytes = (129 + (quote.DirectionCurve ? 33 : 0)) * sizeof(double);
			const uint64_t descriptorBytes =
				sizeof(SourcePathData2D) + sizeof(SourcePathWaveData2D) + tableBytes;
			const uint64_t payloadBytes = MeshAddBytes(childBytes, descriptorBytes);
			quote.Bytes = MeshAddBytes(
				MeshAddBytes(payloadBytes, payloadBytes),
				sizeof(DataReplayEntry) + sizeof(DataReplayValueFrame) +
					std::max(context.Authored.Id.size(), std::string{}.capacity()) +
					std::max<size_t>(4, std::string{}.capacity())
			);
			return quote.Bytes <= Limits::MaximumEvaluationBytes ||
				   context.Fail(Status::LimitExceeded, "Wave clone and replay storage exceed bounds", "path");
		}
		const SourcePathWaveData2D *PreviousWave(NodeContext &context, uint64_t *admittedWork = nullptr) {
			const auto *data = context.CurrentData ? context.CurrentData : context.Request.DataReplay;
			if (!data) return nullptr;
			if (data->Entries.size() > Limits::MaximumArrayElements)
				return context.Fail(Status::LimitExceeded, "Wave replay row count exceeds bounds", "path"),
					   nullptr;
			for (const auto &entry : data->Entries) {
				if (entry.NodeId != context.Authored.Id || entry.ProcessorRow != context.ProcessorRow)
					continue;
				const FrameTime previousTime{entry.Tick, entry.Subframe, entry.NegativeFrame};
				if (!entry.Initialized || !ValidFrameTime(previousTime) ||
					CompareFrameTime(previousTime, GetFrameTime(context.Request)) > 0 ||
					entry.Values.size() != 1)
					return context.Fail(Status::InvalidValue, "Wave replay receipt is invalid", "path"),
						   nullptr;
				const auto *previous = std::get_if<Path2D>(&entry.Values.front().Data);
				const auto work = previous ? SourceWeightRuntimeWork(previous, nullptr) : std::nullopt;
				if (!previous || !previous->SourceOperation ||
					previous->SourceOperation->Kind != SourcePathOperationKind::Wave ||
					!previous->SourceOperation->Wave || !work)
					return context.Fail(Status::InvalidValue, "Wave replay payload is invalid", "path"),
						   nullptr;
				// The preflight and execution both inspect the retained child tree.
				if (admittedWork) {
					if (*admittedWork > WAVE_WORK_LIMIT || *work > (WAVE_WORK_LIMIT - *admittedWork) / 2)
						return context.Fail(
								   Status::LimitExceeded,
								   "Wave retained payload traversal exceeds work bounds",
								   "path"
							   ),
							   nullptr;
					*admittedWork += *work * 2;
				}
				if (!ValidSourcePath2D(*previous))
					return context.Fail(Status::InvalidValue, "Wave replay payload is invalid", "path"),
						   nullptr;
				return previous->SourceOperation->Wave.operator->();
			}
			return nullptr;
		}
		void WaveTable(const Curve &curve, uint64_t precision, std::vector<double> &table) {
			table.reserve(size_t(precision + 1));
			for (uint64_t i = 0; i <= precision; ++i)
				table.push_back(EvalCurveX(curve, double(i) / precision, 1e-5));
		}
	}
	bool AdmitSourcePathWave(NodeContext &context, uint64_t &batchWork, uint64_t &batchBytes) {
		ENGINE_PROFILE("imagegraph.source.path_wave.admission");
		WaveQuote quote;
		if (!QuoteWave(context, quote)) return false;
		PreviousWave(context, &quote.Work);
		if (context.FailureCode != Status::Ok) return false;
		const auto *replay = context.CurrentData ? context.CurrentData : context.Request.DataReplay;
		if (replay) quote.Work = MeshAddBytes(quote.Work, replay->Entries.size());
		if (quote.Work > WAVE_WORK_LIMIT - batchWork)
			return context.Fail(Status::LimitExceeded, "Wave whole processor work exceeds bounds", "path");
		batchWork += quote.Work;
		if (quote.Bytes > context.AvailableBytes() || batchBytes > context.AvailableBytes() - quote.Bytes)
			return context.Fail(Status::LimitExceeded, "Wave whole processor storage exceeds bounds", "path");
		batchBytes += quote.Bytes;
		return true;
	}
	bool ExecuteSourcePathWave(NodeContext &context) try {
		ENGINE_PROFILE("imagegraph.source.path_wave");
		WaveQuote quote;
		if (!QuoteWave(context, quote)) return false;
		const auto *previous = PreviousWave(context, &quote.Work);
		if (context.FailureCode != Status::Ok) return false;
		if (previous) quote.Controls.Buffers = previous->Buffers;
		if (!context.PathShiftMemo)
			return context.Fail(
				Status::UnsupportedExecution, "Wave requires evaluation-owned path memo", "path"
			);
		if (!context.ReserveOutput(quote.Bytes, "path")) return false;
		Path2D output;
		auto &op = output.SourceOperation.emplace();
		op.Kind = SourcePathOperationKind::Wave;
		op.Wave.emplace() = std::move(quote.Controls);
		WaveTable(*quote.AmplitudeCurve, 128, op.Wave->AmplitudeCurve);
		if (quote.DirectionCurve) WaveTable(*quote.DirectionCurve, 32, op.Wave->DirectionCurve);
		if (quote.Planar) op.Inputs.push_back(*quote.Planar);
		if (quote.Spatial) op.WeightInput3D.emplace() = *quote.Spatial;
		if (!ValidSourcePath2D(output))
			return context.Fail(Status::LimitExceeded, "Wave output path payload exceeds bounds", "path");
		op.EvaluationMemoId = context.PathShiftMemo->OwnerId(context, "wave", "path_out");
		if (!op.EvaluationMemoId) return false;
		DataReplayEntry entry;
		entry.NodeId = context.Authored.Id;
		entry.ProcessorRow = context.ProcessorRow;
		entry.Tick = context.Request.Tick;
		entry.Subframe = context.Request.Subframe;
		entry.NegativeFrame = context.Request.NegativeFrame;
		entry.Initialized = true;
		entry.Values.push_back({context.Request.Tick, output});
		context.DataUpdates.reserve(1);
		if (context.DataUpdates.capacity() > 1 &&
			!context.ReserveOutput((context.DataUpdates.capacity() - 1) * sizeof(DataReplayEntry), "path"))
			return false;
		context.SetValue("path", std::move(output));
		if (context.FailureCode != Status::Ok) return false;
		context.DataUpdates.push_back(std::move(entry));
		return true;
	} catch (const std::bad_alloc &) {
		context.ClearOutputs();
		return context.Fail(Status::LimitExceeded, "Wave executor allocation failed");
	} catch (const std::length_error &) {
		context.ClearOutputs();
		return context.Fail(Status::LimitExceeded, "Wave executor container bounds exceeded");
	}
}
