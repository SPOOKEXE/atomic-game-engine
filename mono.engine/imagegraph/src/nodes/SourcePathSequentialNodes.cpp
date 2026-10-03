#include "../SourcePathPayload3D.hpp"
#include "../SourcePathSequentialState.hpp"
#include "Families.hpp"
#include "Path.hpp"

namespace engine::imagegraph::detail {
	namespace {
		using BorrowedPath = std::variant<const Path2D *, const PathData3D *>;
		std::optional<BorrowedPath> SequentialInput(const Value *value) {
			if (!value) return std::nullopt;
			if (const auto *path = std::get_if<Path2D>(value)) {
				return path;
			} else if (const auto *path = std::get_if<PathValue3D>(value);
					   path && path->Data && path->Data->SourcePresent)
				return &*path->Data;
			return std::nullopt;
		}
		uint64_t SequentialInputBytes(const BorrowedPath &input) {
			return std::visit(
				[](const auto *path) {
					using T = std::remove_cv_t<std::remove_pointer_t<decltype(path)>>;
					if constexpr (std::is_same_v<T, Path2D>)
						return MeshAddBytes(sizeof(PathData3D), SourcePath2DBytes<false>(*path));
					else
						return SourcePath3DBytes<false>(*path);
				},
				input
			);
		}
		PathData3D CloneSequentialInput(const BorrowedPath &input) {
			return std::visit(
				[](const auto *path) {
					using T = std::remove_cv_t<std::remove_pointer_t<decltype(path)>>;
					if constexpr (std::is_same_v<T, Path2D>) {
						PathData3D child;
						child.SourcePresent = true;
						child.Source2D = *path;
						return child;
					} else
						return *path;
				},
				input
			);
		}
		const Path2D *PreviousSequentialOutput(NodeContext &context, SourcePathOperationKind kind) {
			const auto *entry = SourceSequentialPrevious(context, context.ProcessorRow);
			if (!entry) return nullptr;
			if (!entry->Initialized || entry->Tick > context.Request.Tick || entry->Values.size() != 1) {
				context.Fail(
					Status::InvalidValue, "Source sequential path replay receipt is invalid", "path"
				);
				return nullptr;
			}
			const auto *path = std::get_if<Path2D>(&entry->Values[0].Data);
			if (!path || !path->SourceOperation || path->SourceOperation->Kind != kind ||
				!ValidSourcePath2D(*path)) {
				context.Fail(
					Status::InvalidValue, "Source sequential path replay payload is invalid", "path"
				);
				return nullptr;
			}
			return path;
		}
		bool PublishSequentialOutput(NodeContext &context, Path2D output) {
			if (!ValidSourcePath2D(output))
				return context.Fail(
					Status::InvalidValue, "Source sequential path output exceeds its bounded contract", "path"
				);
			if (!StampSourcePathShiftOutput(context, output)) return false;
			const uint64_t bytes = MeshAddBytes(
				SourcePath2DBytes<false>(output),
				sizeof(DataReplayEntry) + sizeof(DataReplayValueFrame) +
					std::max(context.Authored.Id.size(), std::string{}.capacity())
			);
			if (!context.ReserveOutput(bytes, "path")) return false;
			DataReplayEntry entry;
			entry.NodeId = context.Authored.Id;
			entry.ProcessorRow = context.ProcessorRow;
			entry.Tick = context.Request.Tick;
			entry.Subframe = context.Request.Subframe;
			entry.NegativeFrame = context.Request.NegativeFrame;
			entry.Initialized = true;
			entry.Values.push_back({context.Request.Tick, output});
			if (RetainedDataReplayEntryBytes(entry) > bytes)
				return context.Fail(
					Status::LimitExceeded, "Source sequential path receipt capacity exceeds admission", "path"
				);
			context.DataUpdates.push_back(std::move(entry));
			context.SetValue("path", std::move(output));
			return context.FailureCode == Status::Ok;
		}
		bool BuildSequentialPath(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.source.path_sequential");
			const bool flatten = context.Authored.Type == "pc.path_flattern",
					   smooth = context.Authored.Type == "pc.path_smoothen";
			const auto kind = flatten  ? SourcePathOperationKind::Flatten
							  : smooth ? SourcePathOperationKind::Smoothen
									   : SourcePathOperationKind::Extends;
			const auto *previous = PreviousSequentialOutput(context, kind);
			if (context.FailureCode != Status::Ok) return false;
			const Value *resolved = context.Find("path");
			if (resolved && !ValidRuntimeValue(*resolved))
				return context.Fail(Status::InvalidValue, "Source sequential path input is invalid", "path");
			const auto defaultPath = context.IsCatalogueDefault("path");
			if (!defaultPath)
				return context.Fail(
					Status::UnsupportedExecution,
					"Source sequential path requires resolved default provenance",
					"path"
				);
			const auto primary = *defaultPath ? std::nullopt : SequentialInput(resolved);
			if (!primary && previous) {
				if (!context.ReserveOutput(SourcePath2DBytes<false>(*previous), "path")) return false;
				Value retained = *previous;
				// Prior receipts enter a fresh journal namespace only after the
				// owned clone has dropped every stale transient identity.
				StripSourcePathShiftIdentities(retained);
				return PublishSequentialOutput(context, std::move(std::get<Path2D>(retained)));
			}
			if (!primary && !smooth) {
				context.SetValue("path", Path2D{});
				return context.FailureCode == Status::Ok;
			}
			std::vector<BorrowedPath> inputs;
			size_t count = primary ? 1 : 0;
			uint64_t payload = primary ? SequentialInputBytes(*primary) : 0;
			if (primary && flatten)
				for (const auto &input : context.Authored.DynamicInputs) {
					const auto *value = context.Find(input.Id);
					if (value && !ValidRuntimeValue(*value))
						return context.Fail(
							Status::InvalidValue, "Source Flatten dynamic input is invalid", input.Id
						);
					if (const auto path = context.IsCatalogueDefault(input.Id).value_or(true)
											  ? std::nullopt
											  : SequentialInput(value)) {
						++count;
						payload = MeshAddBytes(payload, SequentialInputBytes(*path));
					}
				}
			if (count > Limits::MaximumArrayElements || payload > Limits::MaximumEvaluationBytes)
				return context.Fail(
					Status::LimitExceeded, "Source sequential path child payload exceeds bounds", "path"
				);
			auto pointers = context.ReserveWorkspace(count * sizeof(BorrowedPath), "path");
			if (!pointers) return false;
			inputs.reserve(count);
			if (primary) inputs.push_back(*primary);
			if (primary && flatten)
				for (const auto &input : context.Authored.DynamicInputs)
					if (const auto path = context.IsCatalogueDefault(input.Id).value_or(true)
											  ? std::nullopt
											  : SequentialInput(context.Find(input.Id)))
						inputs.push_back(*path);
			const uint64_t bytes = MeshAddBytes(
				payload,
				sizeof(SourcePathData2D) + sizeof(SourcePathSequentialData2D) +
					(flatten ? sizeof(PathData3D) + sizeof(SourcePathData3D) + count * sizeof(PathValue3D)
							 : 0)
			);
			if (!context.ReserveOutput(bytes, "path")) return false;
			Path2D output;
			auto &operation = output.SourceOperation.emplace();
			operation.Kind = kind;
			auto &controls = operation.Sequential.emplace();
			if (flatten) {
				auto &child = operation.WeightInput3D.emplace();
				child.SourcePresent = true;
				auto &combine = child.SourceOperation.emplace();
				combine.Kind = SourcePathOperationKind::Combine;
				combine.Inputs.reserve(count);
				for (const auto &input : inputs) {
					PathValue3D path;
					path.Data.emplace() = CloneSequentialInput(input);
					combine.Inputs.push_back(std::move(path));
				}
			} else if (primary)
				operation.WeightInput3D.emplace() = CloneSequentialInput(*primary);
			if (smooth) {
				if (previous) {
					controls.SmoothPoint = previous->SourceOperation->Sequential->SmoothPoint;
					controls.SmoothProbe = previous->SourceOperation->Sequential->SmoothProbe;
				}
				if (primary) {
					controls.SmoothRange = context.Vec2("range", {0, 1});
					controls.SmoothClampCurve = context.Boolean("clamp_curve");
					controls.SmoothLoop = context.Boolean("loop");
					controls.SmoothSpan = context.Scalar("span", .02);
					controls.SmoothBlend = context.Scalar("blend", 1);
					controls.SmoothSteps = context.Integer("step", 1);
				} else {
					controls.SmoothSpan = 0;
					controls.SmoothBlend = 0;
				}
			} else {
				SourcePathWeightRuntime3D runtime(context, *operation.WeightInput3D);
				if (!runtime.Valid()) return false;
				if (flatten) {
					size_t lines = 0;
					for (size_t i = 0; i < count; ++i) {
						const auto childLines = runtime.OriginalChildLineCount(i);
						if (childLines > Limits::MaximumArrayElements - lines)
							return context.Fail(
								Status::LimitExceeded, "Source Flatten line table exceeds bounds", "path"
							);
						lines += childLines;
					}
					if (!context.ReserveOutput(lines * (2 * sizeof(double) + sizeof(uint32_t)), "path"))
						return false;
					controls.FlattenLengths.reserve(lines);
					controls.FlattenOwners.reserve(lines);
					controls.Accumulated.reserve(lines);
					for (size_t i = 0; i < count; ++i)
						for (size_t line = 0; line < runtime.OriginalChildLineCount(i); ++line) {
							const double length = runtime.OriginalChildLength(i);
							const auto segments = runtime.OriginalChildSegmentCount(i);
							if (segments > Limits::MaximumArrayElements - controls.CachedSegments)
								return context.Fail(
									Status::LimitExceeded, "Source Flatten segments exceed bounds", "path"
								);
							controls.CachedSegments += uint32_t(segments);
							controls.CachedLength += length;
							controls.FlattenLengths.push_back(length);
							controls.FlattenOwners.push_back(uint32_t(i));
							controls.Accumulated.push_back(controls.CachedLength);
						}
					controls.CachedBounds = {-4, -4, -4, -4};
					controls.FlattenReverse = context.Boolean("reverse");
					controls.FlattenPingPong = context.Boolean("ping_pong");
				} else {
					controls.CachedLength = runtime.Length(0);
					const auto segments = runtime.SegmentCount(0), accumulated = runtime.AccumulatedCount(0);
					if (segments >= Limits::MaximumArrayElements ||
						accumulated >= Limits::MaximumArrayElements)
						return context.Fail(
							Status::LimitExceeded, "Source Extends metadata exceeds bounds", "path"
						);
					controls.CachedSegments = uint32_t(segments + 1);
					if (!context.ReserveOutput((accumulated + 1) * sizeof(double), "path")) return false;
					controls.Accumulated.reserve(accumulated + 1);
					controls.Accumulated.push_back(0);
					for (size_t i = 0; i < accumulated; ++i)
						controls.Accumulated.push_back(runtime.AccumulatedAt(i, 0));
					const auto bounds = runtime.Boundary();
					if (!bounds)
						return context.Fail(
							Status::UnsupportedExecution,
							"Source Extends requires the child's captured source bounds",
							"path"
						);
					controls.CachedBounds = *bounds;
					SourcePathPointBuffer buffer;
					const auto start = runtime.RatioInto(0, 0, buffer);
					buffer = {};
					const auto startNext = runtime.RatioInto(.001, 0, buffer);
					buffer = {};
					const auto end = runtime.RatioInto(.999, 0, buffer);
					buffer = {};
					const auto endPrevious = runtime.RatioInto(.998, 0, buffer);
					controls.ExtendStartPoint = {start.Position, start.Weight};
					controls.ExtendEndPoint = {end.Position, end.Weight};
					controls.ExtendStartDirection = SourceWeightDirection(
						start.Position.X - startNext.Position.X, start.Position.Y - startNext.Position.Y
					);
					controls.ExtendEndDirection = SourceWeightDirection(
						end.Position.X - endPrevious.Position.X, end.Position.Y - endPrevious.Position.Y
					);
					controls.CachedBounds.X = std::min(controls.CachedBounds.X, start.Position.X);
					controls.CachedBounds.Y = std::min(controls.CachedBounds.Y, start.Position.Y);
					controls.CachedBounds.Z = std::max(controls.CachedBounds.Z, start.Position.X);
					controls.CachedBounds.W = std::max(controls.CachedBounds.W, start.Position.Y);
					const double side = context.SourceChoice("side");
					if (side != 0 && side != 1)
						return context.Fail(Status::InvalidValue, "Source Extends side is undefined", "side");
					controls.ExtendSide = uint8_t(side);
					controls.ExtendLength = context.Scalar("length", 16);
				}
			}
			if (context.FailureCode != Status::Ok) return false;
			return PublishSequentialOutput(context, std::move(output));
		}
	}
	std::span<const ExecutorEntry> SourcePathSequentialExecutors() {
		static constexpr std::array ENTRIES{
			ExecutorEntry{"pc.path_extends", BuildSequentialPath},
			ExecutorEntry{"pc.path_flattern", BuildSequentialPath},
			ExecutorEntry{"pc.path_smoothen", BuildSequentialPath, true}
		};
		return ENTRIES;
	}
}
