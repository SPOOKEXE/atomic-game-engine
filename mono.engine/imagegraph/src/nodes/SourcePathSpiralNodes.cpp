#include "SourcePathSpiralNodes.hpp"

#include "../SourcePathPayload3D.hpp"
#include "../SourcePathShiftMemo.hpp"
#include "Curve.hpp"

namespace engine::imagegraph::detail {
	namespace source_path_spiral {
		constexpr uint64_t SPIRAL_WORK_LIMIT = uint64_t{1} << 24;
		bool Work3D(const PathData3D &path, uint64_t &work, size_t depth);
		bool TableWork(size_t count, uint64_t &work, uint64_t perItem = 1) {
			if (count > Limits::MaximumArrayElements || count > (SPIRAL_WORK_LIMIT - work) / perItem)
				return false;
			work += count * perItem;
			return true;
		}
		bool Work2D(const Path2D &path, uint64_t &work, size_t depth = 0) {
			if (depth > Limits::MaximumArrayDepth || path.Anchors.size() > Limits::MaximumPathAnchors ||
				path.Weights.size() > Limits::MaximumArrayElements)
				return false;
			const uint64_t add = 1 + path.Anchors.size() * 128 + path.Weights.size();
			if (add > SPIRAL_WORK_LIMIT - work) return false;
			work += add;
			if (!path.SourceOperation) return true;
			const auto &op = *path.SourceOperation;
			if (op.Inputs.size() > Limits::MaximumArrayElements) return false;
			if (!TableWork(op.WeightCurve.size(), work) || !TableWork(op.CachedLengths.size(), work) ||
				!TableWork(op.Reversed.size(), work) || !TableWork(op.BlendLengths.size(), work) ||
				!TableWork(op.BlendAccumulated.size(), work))
				return false;
			for (const auto &table : op.BlendAccumulated)
				if (!TableWork(table.size(), work)) return false;
			if (op.Spiral && (!TableWork(op.Spiral->AmplitudeCurve.size(), work) ||
							  !TableWork(op.Spiral->DirectionCurve.size(), work) ||
							  !TableWork(op.Spiral->Cache.size(), work, 4096)))
				return false;
			uint64_t childRepeats = op.Spiral ? (op.Spiral->Direction == 0 ? 6 : 4) : 1;
			if (op.Sequential) {
				const auto &sequential = *op.Sequential;
				if (!TableWork(sequential.Accumulated.size(), work) ||
					!TableWork(sequential.FlattenLengths.size(), work) ||
					!TableWork(sequential.FlattenOwners.size(), work) ||
					!TableWork(sequential.Cache.size(), work, 4096))
					return false;
				if (op.Kind == SourcePathOperationKind::Smoothen) {
					if (sequential.SmoothSteps > int64_t(Limits::MaximumArrayElements)) return false;
					childRepeats += 2 * uint64_t(std::max<int64_t>(sequential.SmoothSteps, 0));
				}
			}
			if (op.WeightInput3D) {
				uint64_t childWork = 0;
				if (!Work3D(*op.WeightInput3D, childWork, depth + 1) ||
					childWork > (SPIRAL_WORK_LIMIT - work) / childRepeats)
					return false;
				work += childWork * childRepeats;
			}
			const uint64_t extra =
				(op.Shape ? op.Shape->Points.size() : 0) + (op.Baked ? op.Baked->Lines.size() : 0);
			if (extra > SPIRAL_WORK_LIMIT - work) return false;
			work += extra;
			if (op.Baked) {
				if (4096 > SPIRAL_WORK_LIMIT - work) return false;
				work += 4096;
			}
			if (op.Baked)
				for (const auto &line : op.Baked->Lines) {
					if (line.size() > SPIRAL_WORK_LIMIT - work) return false;
					work += line.size();
				}
			if (op.Mesh) {
				const uint64_t edges = op.Mesh->Simulation.Edges.size();
				if (edges > SPIRAL_WORK_LIMIT - work) return false;
				work += edges;
			}
			for (const auto &child : op.Inputs) {
				uint64_t childWork = 0;
				if (!Work2D(child, childWork, depth + 1) ||
					childWork > (SPIRAL_WORK_LIMIT - work) / childRepeats)
					return false;
				work += childWork * childRepeats;
			}
			return true;
		}
		bool Work3D(const PathData3D &path, uint64_t &work, size_t depth = 0) {
			if (depth > Limits::MaximumArrayDepth || path.Anchors.size() > Limits::MaximumPathAnchors ||
				path.Resolution > Limits::MaximumArrayElements ||
				path.Transforms.size() > Limits::MaximumArrayDepth)
				return false;
			const uint64_t add =
				1 + path.Anchors.size() * (uint64_t(path.Resolution) + 1) * 32 + path.Transforms.size() * 64;
			if (add > SPIRAL_WORK_LIMIT - work) return false;
			work += add;
			if (path.Source2D && !Work2D(*path.Source2D, work, depth + 1)) return false;
			if (path.SourceOperation) {
				if (path.SourceOperation->Inputs.size() > Limits::MaximumArrayElements) return false;
				for (const auto &child : path.SourceOperation->Inputs)
					if (!child.Data || !Work3D(*child.Data, work, depth + 1)) return false;
			}
			return true;
		}

		struct Quote {
			uint64_t Bytes = 0, Work = 0, ScanWork = 0;
		};
		template <class Leaf> bool Visit(const Value &value, Leaf &&leaf) {
			const auto *array = std::get_if<ArrayValue>(&value);
			if (!array) return leaf(value);
			if (array->Elements.size() > Limits::MaximumArrayElements ||
				array->Nested.size() > Limits::MaximumArrayElements ||
				array->Items.size() > Limits::MaximumArrayElements)
				return false;
			size_t count = 0;
			for (const auto &item : array->Elements) {
				if (++count > Limits::MaximumArrayElements || !leaf(item)) return false;
			}
			for (const auto &row : array->Nested) {
				if (row.size() > Limits::MaximumArrayElements - count) return false;
				count += row.size();
				for (const auto &item : row)
					if (!leaf(item)) return false;
			}
			const auto visitItem = [&](auto &&self, const SourceArrayItem &item, size_t depth) -> bool {
				if (depth > Limits::MaximumArrayDepth || ++count > Limits::MaximumArrayElements) return false;
				if (const auto *itemLeaf = std::get_if<ElementValue>(&item.Data)) return leaf(*itemLeaf);
				const auto *children = std::get_if<std::vector<SourceArrayItem>>(&item.Data);
				if (!children || children->size() > Limits::MaximumArrayElements - count) return false;
				for (const auto &child : *children)
					if (!self(self, child, depth + 1)) return false;
				return true;
			};
			for (const auto &item : array->Items)
				if (!visitItem(visitItem, item, 0)) return false;
			return true;
		}
		template <class V> bool PathQuote(const V &value, Quote &quote) {
			const auto *planar = std::get_if<Path2D>(&value);
			const auto *spatial = std::get_if<PathValue3D>(&value);
			if (std::holds_alternative<UndefinedValue>(value)) return true;
			if (const auto *scalar = std::get_if<double>(&value); scalar && *scalar == -4) return true;
			if (const auto *integer = std::get_if<int64_t>(&value); integer && *integer == -4) return true;
			if (!planar && !spatial) return false;
			if (spatial && !spatial->Data) return true;
			uint64_t work = 0;
			if (!(planar ? Work2D(*planar, work) : Work3D(*spatial->Data, work)) ||
				work > SPIRAL_WORK_LIMIT - quote.ScanWork)
				return false;
			quote.ScanWork += work;
			if (!(planar ? ValidSourcePath2D(*planar) : ValidSourcePath3D(*spatial->Data))) return false;
			const uint64_t bytes = planar ? MeshAddBytes(sizeof(Path2D), SourcePath2DBytes<false>(*planar))
										  : SourcePath3DBytes<false>(*spatial->Data);
			quote.Bytes = std::max(quote.Bytes, bytes);
			quote.Work = std::max(quote.Work, work);
			return bytes <= Limits::MaximumEvaluationBytes;
		}
		template <class V> bool CurveQuote(const V &value, uint64_t samples, uint64_t &work) {
			const auto *curve = std::get_if<Curve>(&value);
			if (!curve || curve->Anchors.size() > Limits::MaximumCurveAnchors) return false;
			work = std::max(work, samples * std::max<size_t>(1, curve->Anchors.size()) * 264);
			return work <= SPIRAL_WORK_LIMIT;
		}
		bool CurveTable(
			NodeContext &context,
			const Curve &curve,
			uint64_t precision,
			std::vector<double> &table,
			std::string_view port
		) {
			table.reserve(size_t(precision + 1));
			for (uint64_t i = 0; i <= precision; ++i) {
				const double sample = EvalCurveX(curve, double(i) / precision, 1e-5);
				if (!std::isfinite(sample))
					return context.Fail(Status::InvalidValue, "Spiral curve sample is nonfinite", port);
				table.push_back(sample);
			}
			return true;
		}
		const SourcePathSpiralData2D *PreviousBuffers(NodeContext &context) {
			const auto *data = context.CurrentData ? context.CurrentData : context.Request.DataReplay;
			if (!data) return nullptr;
			if (data->Entries.size() > Limits::MaximumArrayElements ||
				data->Entries.size() > SPIRAL_WORK_LIMIT / std::max<size_t>(context.ProcessorCount, 1)) {
				context.Fail(Status::LimitExceeded, "Spiral replay row count exceeds bounds", "path");
				return nullptr;
			}
			for (const auto &entry : data->Entries) {
				if (entry.NodeId != context.Authored.Id || entry.ProcessorRow != context.ProcessorRow)
					continue;
				if (!entry.Initialized || entry.Tick > context.Request.Tick || entry.Values.size() != 1) {
					context.Fail(Status::InvalidValue, "Spiral replay receipt is invalid", "path");
					return nullptr;
				}
				const auto *previous = std::get_if<Path2D>(&entry.Values.front().Data);
				uint64_t work = 0;
				if (!previous || !previous->SourceOperation ||
					previous->SourceOperation->Kind != SourcePathOperationKind::Spiral ||
					!previous->SourceOperation->Spiral || !Work2D(*previous, work) ||
					work > SPIRAL_WORK_LIMIT / std::max<size_t>(context.ProcessorCount, 1) ||
					!ValidSourcePath2D(*previous)) {
					context.Fail(Status::InvalidValue, "Spiral replay payload is invalid", "path");
					return nullptr;
				}
				return previous->SourceOperation->Spiral.operator->();
			}
			return nullptr;
		}
		bool Execute(NodeContext &context) try {
			ENGINE_PROFILE("imagegraph.source.path_spiral");
			const auto provenance = context.IsCatalogueDefault("path");
			if (!provenance)
				return context.Fail(
					Status::UnsupportedExecution, "Spiral requires resolved path default provenance", "path"
				);
			const Value *value = context.Find("path");
			const auto *scalar = value ? std::get_if<double>(value) : nullptr;
			const auto *integer = value ? std::get_if<int64_t>(value) : nullptr;
			const bool absent = *provenance || !value || std::holds_alternative<UndefinedValue>(*value) ||
								(scalar && *scalar == -4) || (integer && *integer == -4);
			const auto *planar = value ? std::get_if<Path2D>(value) : nullptr;
			const auto *spatial = value ? std::get_if<PathValue3D>(value) : nullptr;
			Quote path;
			if (!absent && !PathQuote(*value, path))
				return context.Fail(
					Status::LimitExceeded, "Spiral requires a bounded planar or spatial path", "path"
				);
			const bool angleCurved = context.Boolean("angle_curved");
			const Value *ampValue = context.Find("amplitude_curve"), *dirValue = context.Find("angle_curve");
			const auto *ampCurve = ampValue ? std::get_if<Curve>(ampValue) : nullptr;
			const auto *dirCurve = dirValue ? std::get_if<Curve>(dirValue) : nullptr;
			uint64_t amplitudeWork = 0, directionWork = 0;
			if (!ampValue || !CurveQuote(*ampValue, 129, amplitudeWork) ||
				(angleCurved && (!dirValue || !CurveQuote(*dirValue, 33, directionWork))))
				return context.Fail(
					Status::LimitExceeded, "Spiral requires bounded source curves", "amplitude_curve"
				);
			SourcePathSpiralData2D controls;
			const auto *previous = PreviousBuffers(context);
			if (context.FailureCode != Status::Ok) return false;
			if (previous) controls.Buffers = previous->Buffers;
			controls.Range = context.Vec2("range", {0, 1});
			controls.DirectionRange = context.Vec2("angle", {90, 90});
			controls.WeightRange = context.Vec2("range_2", {0, 1});
			controls.ClampCurve = context.Boolean("clamp_curve");
			controls.Loop = context.Boolean("loop");
			controls.UseWeight = context.Boolean("use_weight");
			controls.Frequency = context.Scalar("frequency", 4);
			controls.Amplitude = context.Scalar("amplitude", 4);
			controls.Spiral = context.Scalar("spiral", .75);
			controls.Phase = context.Scalar("phase");
			const double direction = context.SourceChoice("direction"),
						 mode = context.SourceChoice("weight_mode");
			if (context.FailureCode != Status::Ok) return false;
			if (!MeshFinite(controls.Range) || !MeshFinite(controls.DirectionRange) ||
				!MeshFinite(controls.WeightRange) || !std::isfinite(controls.Frequency) ||
				!std::isfinite(controls.Amplitude) || !std::isfinite(controls.Spiral) ||
				!std::isfinite(controls.Phase) || direction < 0 || direction > 1 ||
				std::trunc(direction) != direction || mode < 0 || mode > 2 || std::trunc(mode) != mode)
				return context.Fail(
					Status::InvalidValue, "Spiral controls must be finite and supported", "direction"
				);
			controls.Direction = uint8_t(direction);
			controls.WeightMode = uint8_t(mode);
			if (context.ProcessorRow == 0) {
				for (const auto &[port, original] : context.ProcessorOriginalValues) {
					if (!original) continue;
					bool valid = true;
					if (port == "path")
						valid = Visit(*original, [&](const auto &leaf) { return PathQuote(leaf, path); });
					if (port == "amplitude_curve")
						valid = Visit(*original, [&](const auto &leaf) {
							return CurveQuote(leaf, 129, amplitudeWork);
						});
					if (port == "angle_curve")
						valid = Visit(*original, [&](const auto &leaf) {
							return CurveQuote(leaf, 33, directionWork);
						});
					if (!valid)
						return context.Fail(
							Status::LimitExceeded, "Spiral processor rows exceed bounded payloads", port
						);
				}
			}
			const uint64_t samples = 162;
			const uint64_t bytes = MeshAddBytes(
				path.Bytes,
				sizeof(SourcePathData2D) + sizeof(SourcePathSpiralData2D) + samples * sizeof(double)
			);
			const uint64_t receiptOverhead = sizeof(DataReplayEntry) + sizeof(DataReplayValueFrame) +
											 std::max(context.Authored.Id.size(), std::string{}.capacity());
			const uint64_t combinedBytes = MeshAddBytes(MeshAddBytes(bytes, bytes), receiptOverhead);
			const uint64_t rows = std::max<size_t>(context.ProcessorCount, 1);
			const uint64_t work = MeshAddBytes(path.Work, MeshAddBytes(amplitudeWork, directionWork));
			if (combinedBytes > Limits::MaximumEvaluationBytes ||
				rows > Limits::MaximumEvaluationBytes / combinedBytes || work > SPIRAL_WORK_LIMIT / rows)
				return context.Fail(
					Status::LimitExceeded, "Spiral aggregate clone or curve work exceeds bounds", "path"
				);
			if (context.ProcessorRow == 0) {
				auto charge = context.ReserveWorkspace(combinedBytes * rows, "path");
				if (!charge) return false;
			}
			if (!context.ReserveOutput(combinedBytes, "path")) return false;
			Path2D output;
			auto &op = output.SourceOperation.emplace();
			op.Kind = SourcePathOperationKind::Spiral;
			op.Spiral.emplace() = std::move(controls);
			if (!CurveTable(context, *ampCurve, 128, op.Spiral->AmplitudeCurve, "amplitude_curve") ||
				(angleCurved &&
				 !CurveTable(context, *dirCurve, 32, op.Spiral->DirectionCurve, "angle_curve")))
				return false;
			if (!absent) {
				if (spatial && spatial->Data)
					op.WeightInput3D = spatial->Data;
				else if (planar)
					op.Inputs.push_back(*planar);
			}
			if (!ValidSourcePath2D(output))
				return context.Fail(
					Status::LimitExceeded, "Spiral output exceeds bounded path payload", "path"
				);
			if (!context.PathShiftMemo)
				return context.Fail(
					Status::UnsupportedExecution, "Spiral requires evaluation-owned path memo", "path"
				);
			op.EvaluationMemoId = context.PathShiftMemo->OwnerId(context, "spiral", "path_out");
			if (!op.EvaluationMemoId) return false;
			DataReplayEntry entry;
			entry.NodeId = context.Authored.Id;
			entry.ProcessorRow = context.ProcessorRow;
			entry.Tick = context.Request.Tick;
			entry.Subframe = context.Request.Subframe;
			entry.NegativeFrame = context.Request.NegativeFrame;
			entry.Initialized = true;
			entry.Values.push_back({context.Request.Tick, output});
			if (RetainedDataReplayEntryBytes(entry) > bytes + receiptOverhead)
				return context.Fail(
					Status::LimitExceeded, "Spiral replay receipt exceeds admitted storage", "path"
				);
			// Receipt admission includes the update container's first entry.
			context.DataUpdates.reserve(1);
			if (context.DataUpdates.capacity() > 1 &&
				!context.ReserveOutput(
					(context.DataUpdates.capacity() - 1) * sizeof(DataReplayEntry), "path"
				))
				return false;
			context.SetValue("path", std::move(output));
			if (context.FailureCode != Status::Ok) return false;
			context.DataUpdates.push_back(std::move(entry));
			return true;
		} catch (const std::bad_alloc &) {
			context.ClearOutputs();
			return context.Fail(Status::LimitExceeded, "Spiral executor allocation failed");
		} catch (const std::length_error &) {
			context.ClearOutputs();
			return context.Fail(Status::LimitExceeded, "Spiral executor container bounds exceeded");
		}
	}
	std::span<const ExecutorEntry> SourcePathSpiralExecutors() {
		static constexpr std::array ENTRIES{
			ExecutorEntry{"pc.path_spiral", source_path_spiral::Execute, true}
		};
		return ENTRIES;
	}
}
