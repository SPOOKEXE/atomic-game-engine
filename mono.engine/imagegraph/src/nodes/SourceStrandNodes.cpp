#include "SourceStrandNodes.hpp"

#include "SourceBuiltinRandomContext.hpp"
#include "StrandReplayInternal.hpp"
namespace engine::imagegraph::detail {
	bool StrandCreate(NodeContext &context) {
		ENGINE_PROFILE("imagegraph.strand.create");
		if (context.Request.Subframe != 0 || context.Request.NegativeFrame)
			return context.Fail(
				Status::UnsupportedExecution, "Strand simulation requires nonnegative integral frames"
			);
		if (context.SourceChoice("source") != 0 || context.SourceChoice("distribution") != 0 ||
			context.Boolean("attribute_use_groom") || context.Boolean("bake_hair"))
			return context.Fail(
				Status::UnsupportedExecution,
				"Strand Create currently supports unbaked uniform Point geometry only"
			);
		SourceStrandCreateSettings settings;
		const double count = context.Scalar("strands", 8), segments = context.Scalar("segment", 4);
		if (!std::isfinite(count) || !std::isfinite(segments) || count != std::trunc(count) ||
			segments != std::trunc(segments) || count > Limits::MaximumArrayElements || segments < 0 ||
			segments >= MAXIMUM_SOURCE_STRAND_POINTS)
			return context.Fail(Status::InvalidValue, "Strand counts must be bounded integers");
		settings.Hairs = uint32_t(std::max(0., count));
		settings.Segments = uint32_t(segments);
		const auto position = context.Vec2("position", {.5, .5}), length = context.Vec2("length", {4, 4}),
				   root = context.Vec2("root_strength", {-1, -1});
		settings.Position = {position.X, position.Y};
		settings.Length = {length.X, length.Y};
		settings.RootStrength = {root.X, root.Y};
		if (std::find(context.LinkedValues.begin(), context.LinkedValues.end(), "position") ==
				context.LinkedValues.end() &&
			context.SourceChoice("position_unit", 1) == 1) {
			settings.Position[0] *= context.Project.SurfaceWidth;
			settings.Position[1] *= context.Project.SurfaceHeight;
		}
		settings.Elasticity = context.Scalar("elasticity", .05);
		settings.Spring = context.Scalar("spring", .8);
		settings.Structure = context.Scalar("structure", .2);
		settings.Restitution = context.Scalar("restitution", .01);
		settings.CurlFrequency = context.Scalar("curl_frequency");
		settings.CurlSize = context.Scalar("curliness", 1);
		if (context.FailureCode != Status::Ok) return false;
		const DataReplayEntry *prior =
			FindStrandReplayEntry(context, context.Authored.Id, context.ProcessorRow);
		const StrandValue *previous =
			prior && prior->Values.size() == 1 ? std::get_if<StrandValue>(&prior->Values[0].Data) : nullptr;
		if (prior && (!ValidStrandReplayEntry(*prior) || !previous || !previous->Data ||
					  previous->Data->AuthoringRevision != context.Request.SimulationAuthoringRevision))
			return context.Fail(Status::InvalidValue, "Strand prior revision requires explicit replay reset");
		if (context.Request.ReuseSimulationFrame) {
			if (!prior || !StrandReplayTimeMatches(*prior, context.Request))
				return context.Fail(
					Status::InvalidValue, "Strand captured frame does not match current tick"
				);
			if (!context.ReserveOutput(StrandStorageBytes<true>(*previous), "strands")) return false;
			context.SetValue("strands", *previous);
			return context.FailureCode == Status::Ok;
		}
		if (prior && (prior->NegativeFrame || prior->Subframe != 0 || prior->Tick >= Limits::MaximumTick ||
					  prior->Tick + 1 != context.Request.Tick))
			return context.Fail(
				Status::InvalidValue, "Strand Create requires contiguous ticks or explicit reset"
			);
		if (!prior && context.Request.Tick != 0)
			return context.Fail(Status::InvalidValue, "Strand Create requires tick-zero initialization");
		const uint32_t existing = previous ? previous->Data->State.Hairs.size() : 0;
		const SourceBuiltinRandomCapture *capture = nullptr;
		if (settings.Hairs > existing && !FindSourceBuiltinRandomCapture(context, capture)) return false;
		const uint64_t old = previous ? StrandStorageBytes<true>(*previous) : 0;
		const uint64_t bound = MeshAddBytes(
			sizeof(StrandData2D) + std::max(context.Authored.Id.size(), std::string{}.capacity()),
			MeshAddBytes(
				old,
				uint64_t(settings.Hairs > existing ? settings.Hairs - existing : 0) *
					(sizeof(SourceStrandHair) +
					 uint64_t(settings.Segments + 1) * (sizeof(SourceStrandPoint) + 2 * sizeof(double)))
			)
		);
		if (!context.ReserveOutput(bound, "strands")) return false;
		StrandValue output;
		auto &data = output.Data.emplace();
		data.OriginNodeId = context.Authored.Id;
		data.OriginProcessorRow = context.ProcessorRow;
		data.AuthoringRevision = context.Request.SimulationAuthoringRevision;
		Diagnostic diagnostic;
		const auto draws = capture ? std::span<const SourceBuiltinRandomDraw>(capture->Draws)
								   : std::span<const SourceBuiltinRandomDraw>{};
		if (SourceStrandCreateUniformPoint(
				settings,
				draws,
				Limits::MaximumEvaluationBytes,
				data.State,
				diagnostic,
				previous ? &previous->Data->State : nullptr
			) != Status::Ok)
			return context.Fail(diagnostic);
		if (!PublishStrandReplay(context, output)) return false;
		context.SetValue("strands", std::move(output));
		return context.FailureCode == Status::Ok;
	}
	namespace {
		bool MutateStrand(NodeContext &context, bool update) {
			const auto *input = context.Find("input_0");
			if (!input) {
				context.SetValue("strands", StrandValue{});
				return context.FailureCode == Status::Ok;
			}
			const double authoredSteps = update ? context.Scalar("step", 4) : 0;
			if (!std::isfinite(authoredSteps) || authoredSteps > 4096 ||
				authoredSteps != std::trunc(authoredSteps))
				return context.Fail(Status::InvalidValue, "Strand Step requires bounded integer");
			uint64_t totalPoints = 0;
			const auto countPoints = [&](const StrandValue &value) {
				const auto *resolved = FindStrandReplayValue(context, value);
				const auto &current = resolved ? *resolved : value;
				if (current.Data)
					for (const auto &hair : current.Data->State.Hairs)
						totalPoints = MeshAddBytes(totalPoints, hair.Points.size());
			};
			if (const auto *strand = std::get_if<StrandValue>(input))
				countPoints(*strand);
			else if (const auto *array = std::get_if<ArrayValue>(input)) {
				for (const auto &element : array->Elements)
					if (const auto *strand = std::get_if<StrandValue>(&element)) countPoints(*strand);
				for (const auto &item : array->Items)
					if (const auto *element = std::get_if<ElementValue>(&item.Data))
						if (const auto *strand = std::get_if<StrandValue>(element)) countPoints(*strand);
			}
			const uint64_t steps = uint64_t(std::max(0., authoredSteps));
			if (steps && totalPoints > MAXIMUM_SOURCE_STRAND_WORK / (steps * 12))
				return context.Fail(
					Status::LimitExceeded, "Strand array update exceeds bounded constraint visits"
				);
			if (!context.ReserveOutput(RetainedPayloadBytes(*input), "strands")) return false;
			Value output = *input;
			const auto mutate = [&](StrandValue &value) {
				if (!value.Data) return true;
				if (const auto *latest = FindStrandReplayValue(context, value)) {
					if (!context.ReserveOutput(StrandStorageBytes<true>(*latest), "strands")) return false;
					value = *latest;
				}
				if (value.Data->AuthoringRevision != context.Request.SimulationAuthoringRevision)
					return context.Fail(
						Status::InvalidValue, "Strand mutation revision requires explicit replay reset"
					);
				const auto *entry =
					FindStrandReplayEntry(context, value.Data->OriginNodeId, value.Data->OriginProcessorRow);
				if (!entry || !StrandReplayTimeMatches(*entry, context.Request))
					return context.Fail(
						Status::UnsupportedExecution,
						"Strand mutation requires current stateful creator snapshot"
					);
				if (context.Request.ReuseSimulationFrame) return true;
				if (!context.ReserveOutput(StrandStorageBytes<true>(value), "strands")) return false;
				Diagnostic diagnostic;
				SourceStrandState next;
				Status status;
				if (update) {
					const double step = context.Scalar("step", 4);
					if (!std::isfinite(step) || step > 4096 || step != std::trunc(step))
						return context.Fail(Status::InvalidValue, "Strand Step requires bounded integer");
					// StrandMesh.step forwards only Step; the source ignores authored
					// Iteration.
					status = SourceStrandUpdate(
						value.Data->State,
						uint32_t(std::max(0., step)),
						Limits::MaximumEvaluationBytes,
						next,
						diagnostic
					);
				} else
					status = SourceStrandGravity(
						value.Data->State,
						context.Scalar("gravity", 1),
						context.Scalar("direction", -90),
						Limits::MaximumEvaluationBytes,
						next,
						diagnostic
					);
				if (status != Status::Ok) return context.Fail(diagnostic);
				value.Data->State = std::move(next);
				return PublishStrandReplay(context, value);
			};
			if (auto *strand = std::get_if<StrandValue>(&output)) {
				if (!mutate(*strand)) return false;
			} else if (auto *array = std::get_if<ArrayValue>(&output)) {
				if ((array->ElementType != ValueType::Strand && array->ElementType != ValueType::Any) ||
					!array->Nested.empty())
					return context.Fail(
						Status::UnsupportedExecution, "Strand mutation requires flat typed Strand array"
					);
				for (auto &element : array->Elements) {
					auto *strand = std::get_if<StrandValue>(&element);
					if (!strand)
						return context.Fail(
							Status::InvalidValue, "Strand array requires source Strand leaves"
						);
					if (!mutate(*strand)) return false;
				}
				for (auto &item : array->Items) {
					auto *element = std::get_if<ElementValue>(&item.Data);
					auto *strand = element ? std::get_if<StrandValue>(element) : nullptr;
					if (!strand)
						return context.Fail(
							Status::InvalidValue, "Strand array requires source Strand leaves"
						);
					if (!mutate(*strand)) return false;
				}
			} else
				return context.Fail(Status::InvalidValue, "Strand input requires typed Strand geometry");
			context.SetValue("strands", std::move(output));
			return context.FailureCode == Status::Ok;
		}
	} // namespace
	bool StrandGravity(NodeContext &context) {
		ENGINE_PROFILE("imagegraph.strand.gravity");
		return MutateStrand(context, false);
	}
	bool StrandUpdate(NodeContext &context) {
		ENGINE_PROFILE("imagegraph.strand.update");
		return MutateStrand(context, true);
	}
} // namespace engine::imagegraph::detail
