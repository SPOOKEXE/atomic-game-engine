// Bridge owns sampled source lines, including source weights and the asymmetric smooth controls.
#include "../SourceBuiltinRandomContext.hpp"
#include "../SourcePathPayload3D.hpp"
#include "Families.hpp"
#include "Path.hpp"

#include <engine/imagegraph/FrameTime.hpp>

namespace engine::imagegraph::detail {
	namespace {
		struct BridgeInput {
			std::optional<PathRuntime> Planar;
			std::optional<SourcePathWeightRuntime3D> Spatial;
			size_t Lines = 0;
			uint64_t Work = 0;
			bool Init(NodeContext &context, const Value &value, uint64_t &initializationWork) {
				if (const auto *path = std::get_if<Path2D>(&value)) {
					const auto work = SourceWeightRuntimeWork(path, nullptr);
					if (!work)
						return context.Fail(Status::LimitExceeded, "Bridge child work is unbounded", "path");
					if (*work > 64000000 - initializationWork)
						return context.Fail(
							Status::LimitExceeded, "Bridge child initialization exceeds work bounds", "path"
						);
					initializationWork += *work;
					Work = *work;
					Planar.emplace();
					if (!Planar->Init(context, *path)) return false;
					Lines = Planar->LineCount();
					return true;
				}
				const auto *path = std::get_if<PathValue3D>(&value);
				if (!path || !path->Data || !path->Data->SourcePresent) return false;
				const auto work = SourceWeightRuntimeWork(nullptr, &*path->Data);
				if (!work)
					return context.Fail(Status::LimitExceeded, "Bridge child work is unbounded", "path");
				if (*work > 64000000 - initializationWork)
					return context.Fail(
						Status::LimitExceeded, "Bridge child initialization exceeds work bounds", "path"
					);
				initializationWork += *work;
				Work = *work;
				Spatial.emplace(context, *path->Data);
				if (!Spatial->Valid()) return false;
				Lines = Spatial->LineCount();
				return true;
			}
			void Sample(double ratio, size_t line, SourcePathPointBuffer &point) const {
				if (Planar)
					point = Planar->PointRatioInto(ratio, line, point);
				else
					point = Spatial->RatioInto(ratio, line, point);
			}
		};
		bool BridgePathPresent(NodeContext &context, std::string_view port) {
			const auto provenance = context.IsCatalogueDefault(port);
			if (!provenance)
				return context.Fail(
					Status::UnsupportedExecution,
					"Bridge requires resolved source path default provenance",
					port
				);
			if (*provenance) return false;
			const auto *value = context.Find(port);
			if (!value) return false;
			if (const auto *path = std::get_if<Path2D>(value)) return ValidSourcePath2D(*path);
			if (const auto *path = std::get_if<PathValue3D>(value))
				return path->Data && path->Data->SourcePresent && ValidSourcePath3D(*path->Data);
			return false;
		}
		const Path2D *PreviousBridge(NodeContext &context) {
			const auto *owner = context.CurrentData ? context.CurrentData : context.Request.DataReplay;
			if (!owner) return nullptr;
			Diagnostic diagnostic;
			if (ValidateDataReplay(*owner, context.ByteBudget, diagnostic) != Status::Ok) {
				context.Fail(diagnostic);
				return nullptr;
			}
			for (const auto &entry : owner->Entries) {
				if (entry.NodeId != context.Authored.Id || entry.ProcessorRow != context.ProcessorRow)
					continue;
				if (entry.Values.size() != 1 || entry.Values[0].Frame != entry.Tick ||
					CompareFrameTime(
						{entry.Tick, entry.Subframe, entry.NegativeFrame}, GetFrameTime(context.Request)
					) > 0) {
					context.Fail(Status::InvalidValue, "Bridge replay receipt has an invalid frame", "path");
					return nullptr;
				}
				const auto *path = std::get_if<Path2D>(&entry.Values[0].Data);
				if (!path || !path->SourceOperation ||
					path->SourceOperation->Kind != SourcePathOperationKind::Bridge ||
					!ValidSourcePath2D(*path)) {
					context.Fail(Status::InvalidValue, "Bridge replay payload is invalid", "path");
					return nullptr;
				}
				return path;
			}
			return nullptr;
		}
		bool PublishBridge(NodeContext &context, Path2D output) {
			if (!ValidSourcePath2D(output))
				return context.Fail(
					Status::InvalidValue, "Bridge output exceeds its source payload contract", "path"
				);
			DataReplayEntry receipt;
			receipt.NodeId = context.Authored.Id;
			receipt.ProcessorRow = context.ProcessorRow;
			receipt.Tick = context.Request.Tick;
			receipt.Subframe = context.Request.Subframe;
			receipt.NegativeFrame = context.Request.NegativeFrame;
			receipt.Initialized = true;
			receipt.Values.reserve(1);
			receipt.Values.push_back({context.Request.Tick, Value{output}});
			context.DataUpdates.reserve(context.DataUpdates.size() + 1);
			context.SetValue("path", std::move(output));
			if (context.FailureCode != Status::Ok) return false;
			context.DataUpdates.push_back(std::move(receipt));
			return true;
		}
		uint64_t BridgeReceiptBytes(NodeContext &context) {
			return sizeof(DataReplayEntry) + sizeof(DataReplayValueFrame) +
				   std::max(context.Authored.Id.size(), std::string{}.capacity()) + std::string{}.capacity();
		}
		bool BridgeBoolean(NodeContext &context, std::string_view port) {
			const auto *value = context.Find(port);
			if (!value) return false;
			if (const auto *flag = std::get_if<bool>(value)) return *flag;
			if (const auto number = SourceChoiceNumber(*value)) return *number > .5;
			context.Fail(
				Status::UnsupportedExecution,
				"Bridge Boolean control requires a finite source numeric or Boolean value",
				port
			);
			return false;
		}
		bool SourceBridgePath(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.source.path_bridge");
			const double distribution = context.SourceChoice("distribution");
			const int64_t amount = context.Integer("amount", 4);
			const bool smooth = BridgeBoolean(context, "smooth"), loop = BridgeBoolean(context, "loop");
			const auto *rangeInput = context.Find("range");
			if (rangeInput && std::holds_alternative<ArrayValue>(*rangeInput))
				return context.Fail(
					Status::UnsupportedExecution, "Bridge Range cannot consume a processor array", "range"
				);
			const Vector2 range = context.Vec2("range", {0, 1});
			const double offset = context.Scalar("offset"), seed = context.Scalar("seed");
			if (context.FailureCode != Status::Ok) return false;
			if (!context.Find("seed"))
				return context.Fail(
					Status::UnsupportedExecution, "Bridge requires its resolved source seed", "seed"
				);
			if (!std::isfinite(distribution) || !std::isfinite(range.X) || !std::isfinite(range.Y) ||
				!std::isfinite(offset) || !std::isfinite(seed))
				return context.Fail(Status::InvalidValue, "Bridge controls must be finite", "range");
			if (amount < 0 || uint64_t(amount) > Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "Bridge amount exceeds source array bounds", "amount"
				);
			const auto *previous = PreviousBridge(context);
			if (context.FailureCode != Status::Ok) return false;
			if (!BridgePathPresent(context, "path")) {
				if (context.FailureCode != Status::Ok) return false;
				const uint64_t payload = previous ? SourcePath2DBytes<false>(*previous)
												  : sizeof(SourcePathData2D) + sizeof(SourcePathBridgeData2D);
				if (!context.ReserveOutput(payload * 2 + BridgeReceiptBytes(context), "path")) return false;
				Path2D output = previous ? *previous : Path2D{};
				if (!previous) {
					auto &operation = output.SourceOperation.emplace();
					operation.Kind = SourcePathOperationKind::Bridge;
					operation.Bridge.emplace();
				}
				auto &bridge = *output.SourceOperation->Bridge;
				bridge.LineCount = uint32_t(amount);
				bridge.Smooth = smooth;
				return PublishBridge(context, std::move(output));
			}
			size_t childCount = 1;
			for (const auto &input : context.Authored.DynamicInputs) {
				size_t group = 0;
				const auto *slot = FindDynamicTemplate(context.Entry, input.Id, group);
				if (slot && slot->Id == "path" && BridgePathPresent(context, input.Id)) ++childCount;
				if (context.FailureCode != Status::Ok) return false;
			}
			auto scratch = context.ReserveWorkspace(childCount * sizeof(BridgeInput), "path");
			if (!scratch) return false;
			std::vector<BridgeInput> children;
			children.reserve(childCount);
			uint64_t initializationWork = 0;
			const auto add = [&](std::string_view port) {
				auto &child = children.emplace_back();
				return child.Init(context, *context.Find(port), initializationWork);
			};
			if (!add("path")) return false;
			for (const auto &input : context.Authored.DynamicInputs) {
				size_t group = 0;
				const auto *slot = FindDynamicTemplate(context.Entry, input.Id, group);
				if (slot && slot->Id == "path" && BridgePathPresent(context, input.Id) && !add(input.Id))
					return false;
				if (context.FailureCode != Status::Ok) return false;
			}
			size_t sourceLines = 0;
			uint64_t childWork = 0;
			for (const auto &child : children) {
				if (child.Lines > Limits::MaximumArrayElements - sourceLines ||
					child.Work > 64000000 - childWork)
					return context.Fail(
						Status::LimitExceeded, "Bridge child lines or work exceed bounds", "path"
					);
				sourceLines += child.Lines;
				childWork += child.Work;
			}
			if (smooth && amount && sourceLines < 2)
				return context.Fail(
					Status::UnsupportedExecution,
					"Source smooth Bridge needs at least two child lines",
					"smooth"
				);
			if (amount && sourceLines == 0)
				return context.Fail(
					Status::UnsupportedExecution, "Source Bridge has no anchor lines", "path"
				);
			const uint64_t vertices = uint64_t(amount) * sourceLines,
						   segments = uint64_t(amount) * (sourceLines ? sourceLines - 1 : 0);
			if ((amount && sourceLines > Limits::MaximumPathAnchors) ||
				vertices + segments * (smooth ? 2 : 1) + uint64_t(amount) >= Limits::MaximumArrayElements ||
				(uint64_t(amount) &&
				 childWork > 64000000 / uint64_t(amount) / std::max<size_t>(1, sourceLines)) ||
				segments > 64000000 / (smooth ? PATH_RESOLUTION : 1))
				return context.Fail(
					Status::LimitExceeded, "Bridge update exceeds whole line or work bounds", "amount"
				);
			const uint64_t payload =
				sizeof(SourcePathData2D) + sizeof(SourcePathBridgeData2D) +
				uint64_t(amount) * sizeof(SourcePathBridgeLine2D) + vertices * sizeof(Vector3) +
				segments * (sizeof(double) + (smooth ? sizeof(std::array<double, 4>) : 0));
			if (!context.ReserveOutput(payload * 2 + BridgeReceiptBytes(context), "path")) return false;
			const SourceBuiltinRandomCapture *capture = nullptr;
			if (distribution == 1 && amount) {
				if (!FindSourceBuiltinRandomCapture(context, capture)) return false;
				if (capture->Draws.size() != size_t(amount))
					return context.Fail(
						Status::InvalidValue,
						"Bridge random observation count differs from source calls",
						"seed"
					);
				for (const auto &draw : capture->Draws)
					if (draw.Operation != SourceBuiltinRandomOperation::Random || draw.Lower != 0 ||
						draw.Upper != 1 || !std::isfinite(draw.Result) || draw.Result < 0 || draw.Result >= 1)
						return context.Fail(
							Status::InvalidValue, "Bridge random observation differs from random(1)", "seed"
						);
			}
			Path2D output;
			auto &operation = output.SourceOperation.emplace();
			operation.Kind = SourcePathOperationKind::Bridge;
			auto &bridge = operation.Bridge.emplace();
			bridge.LineCount = uint32_t(amount);
			bridge.Smooth = smooth;
			bridge.Lines.reserve(size_t(amount));
			SourcePathPointBuffer point;
			for (size_t row = 0; row < size_t(amount); ++row) {
				double ratio = distribution == 0   ? (amount == 1 ? .5 : double(row) / double(amount - !loop))
							   : distribution == 1 ? capture->Draws[row].Result
												   : 0.;
				ratio = range.X + (range.Y - range.X) * ratio;
				if (loop) {
					ratio = ratio + offset;
					ratio = ratio - std::trunc(ratio);
					ratio += 1;
					ratio -= std::trunc(ratio);
				}
				if (!std::isfinite(ratio))
					return context.Fail(Status::InvalidValue, "Bridge source ratio is nonfinite", "range");
				auto &line = bridge.Lines.emplace_back();
				line.Anchors.reserve(sourceLines);
				for (const auto &child : children)
					for (size_t index = 0; index < child.Lines; ++index) {
						child.Sample(std::clamp(ratio, 0., .999), index, point);
						if (context.FailureCode != Status::Ok) return false;
						if (!std::isfinite(point.Position.X) || !std::isfinite(point.Position.Y) ||
							!std::isfinite(point.Weight))
							return context.Fail(
								Status::InvalidValue, "Bridge child sample is nonfinite", "path"
							);
						line.Anchors.push_back({point.Position.X, point.Position.Y, point.Weight});
					}
				const size_t count = sourceLines - 1;
				line.Accumulated.reserve(count);
				if (smooth) {
					line.Controls.resize(count);
					const auto &first = line.Anchors.front(), &last = line.Anchors.back();
					line.Controls.front() = {first.X, first.Y, first.X, first.Y};
					line.Controls.back() = {last.X, last.Y, last.X, last.Y};
					for (size_t index = 1; index + 1 < sourceLines; ++index) {
						const auto &before = line.Anchors[index - 1], &anchor = line.Anchors[index],
								   &after = line.Anchors[index + 1];
						const double direction =
										 SourceWeightDirection(after.X - before.X, after.Y - before.Y) *
										 std::numbers::pi / 180,
									 inDistance = std::hypot(anchor.X - before.X, anchor.Y - before.Y) / 2,
									 outDistance = std::hypot(anchor.X - after.X, anchor.Y - after.Y) / 2;
						line.Controls[index - 1][2] =
							anchor.X - SourceShapeLengthdirComponent(inDistance * std::cos(direction));
						line.Controls[index - 1][3] =
							anchor.Y - SourceShapeLengthdirComponent(-inDistance * std::sin(direction));
						line.Controls[index][0] =
							anchor.X + SourceShapeLengthdirComponent(outDistance * std::cos(direction));
						line.Controls[index][1] =
							anchor.Y + SourceShapeLengthdirComponent(-outDistance * std::sin(direction));
					}
				}
				for (size_t segment = 0; segment < count; ++segment) {
					const auto &a = line.Anchors[segment], &b = line.Anchors[segment + 1];
					double length = 0;
					if (smooth) {
						const auto &controls = line.Controls[segment];
						Vector2 previous;
						for (int step = 0; step < PATH_RESOLUTION; ++step) {
							const double t = double(step) / PATH_RESOLUTION;
							const Vector2 sample{
								BezierComponent(t, a.X, b.X, controls[0], controls[2]),
								BezierComponent(t, a.Y, b.Y, controls[1], controls[3])
							};
							if (step) length += std::hypot(sample.X - previous.X, sample.Y - previous.Y);
							previous = sample;
						}
					} else
						length = std::hypot(b.X - a.X, b.Y - a.Y);
					line.Length += length;
					line.Accumulated.push_back(line.Length);
				}
			}
			return PublishBridge(context, std::move(output));
		}
	}
	std::span<const ExecutorEntry> SourceBridgePathExecutors() {
		static constexpr std::array ENTRIES{ExecutorEntry{"pc.path_bridge", SourceBridgePath, true}};
		return ENTRIES;
	}
}
