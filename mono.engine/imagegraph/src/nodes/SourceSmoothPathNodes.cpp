// Smooth Path owns generated anchors and retains only its source position output across empty updates.

#include "../TimelineDrivers.hpp"
#include "Families.hpp"
#include "Path.hpp"

#include <engine/imagegraph/FrameTime.hpp>

namespace engine::imagegraph::detail {
	namespace {
		template <class T> std::optional<Vector2> SmoothTuple(const T &value) {
			if constexpr (std::is_same_v<T, Vector2> || std::is_same_v<T, Vector3> ||
						  std::is_same_v<T, Vector4> || std::is_same_v<T, Quaternion>)
				return Vector2{value.X, value.Y};
			return std::nullopt;
		}
		template <class Variant> std::optional<double> SmoothNumber(const Variant &value) {
			return std::visit(
				[](const auto &leaf) -> std::optional<double> {
					using T = std::decay_t<decltype(leaf)>;
					if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int64_t> ||
								  std::is_same_v<T, bool>)
						return double(leaf);
					if constexpr (std::is_same_v<T, EnumValue>) return double(leaf.Value);
					return std::nullopt;
				},
				value
			);
		}
		std::optional<Vector2> SmoothPair(std::span<const ElementValue> row) {
			if (row.size() < 2) return std::nullopt;
			const auto x = SmoothNumber(row[0]), y = SmoothNumber(row[1]);
			if (!x || !y) return std::nullopt;
			return Vector2{*x, *y};
		}
		std::optional<Vector2> SmoothPair(std::span<const SourceArrayItem> row) {
			if (row.size() < 2) return std::nullopt;
			const auto *first = std::get_if<ElementValue>(&row[0].Data),
					   *second = std::get_if<ElementValue>(&row[1].Data);
			if (!first || !second) return std::nullopt;
			const auto x = SmoothNumber(*first), y = SmoothNumber(*second);
			if (!x || !y) return std::nullopt;
			return Vector2{*x, *y};
		}
		std::optional<Vector2> SmoothRow(const SourceArrayItem &item) {
			if (const auto *row = std::get_if<std::vector<SourceArrayItem>>(&item.Data))
				return SmoothPair(*row);
			if (const auto *leaf = std::get_if<ElementValue>(&item.Data))
				return std::visit([](const auto &value) { return SmoothTuple(value); }, *leaf);
			return std::nullopt;
		}
		// Source array_get_depth follows only nonempty first children; tuples are source numeric arrays.
		size_t SmoothFirstDepth(const SourceArrayItem &item) {
			if (const auto *row = std::get_if<std::vector<SourceArrayItem>>(&item.Data)) {
				if (row->empty()) return 1;
				if (const auto *first = std::get_if<std::vector<SourceArrayItem>>(&row->front().Data);
					first && first->empty())
					return 1;
				return 1 + SmoothFirstDepth(row->front());
			}
			if (const auto *leaf = std::get_if<ElementValue>(&item.Data))
				return std::visit(
					[](const auto &value) { return size_t(SmoothTuple(value).has_value()); }, *leaf
				);
			return 0;
		}
		template <class Append> bool CollectSmoothAnchors(NodeContext &context, Append append) {
			for (const auto &input : context.Authored.DynamicInputs) {
				size_t group = 0;
				const auto *slot = FindDynamicTemplate(context.Entry, input.Id, group);
				if (!slot || slot->Id != "anchor") continue;
				const auto *value = context.Find(input.Id);
				if (!value) continue;
				if (!ValidRuntimeValue(*value))
					return context.Fail(
						Status::InvalidValue, "Smooth Path anchor payload is invalid", input.Id
					);
				const auto add = [&](std::optional<Vector2> point) {
					if (!point || !std::isfinite(point->X) || !std::isfinite(point->Y))
						return context.Fail(
							Status::UnsupportedExecution,
							"Smooth Path requires finite source coordinate pairs",
							input.Id
						);
					return append(*point, input.Id);
				};
				if (const auto *array = std::get_if<ArrayValue>(value)) {
					if (!array->Items.empty()) {
						const auto *first =
							std::get_if<std::vector<SourceArrayItem>>(&array->Items.front().Data);
						const size_t depth =
							first && first->empty() ? 1 : 1 + SmoothFirstDepth(array->Items.front());
						if (depth == 1) {
							if (!add(SmoothPair(array->Items))) return false;
						} else if (depth == 2)
							for (const auto &row : array->Items)
								if (!add(SmoothRow(row))) return false;
					} else if (!array->Nested.empty()) {
						if (array->Nested.front().empty()) {
							return context.Fail(
								Status::UnsupportedExecution,
								"Smooth Path first empty row leaves undefined coordinates",
								input.Id
							);
						}
						// A tuple nested inside the first numeric row makes depth three, which source
						// ignores.
						if (std::visit(
								[](const auto &leaf) { return SmoothTuple(leaf).has_value(); },
								array->Nested.front().front()
							))
							continue;
						for (const auto &row : array->Nested)
							if (!add(SmoothPair(row))) return false;
					} else if (!array->Elements.empty() &&
							   std::visit(
								   [](const auto &leaf) { return SmoothTuple(leaf).has_value(); },
								   array->Elements.front()
							   )) {
						for (const auto &leaf : array->Elements)
							if (!add(std::visit([](const auto &item) { return SmoothTuple(item); }, leaf)))
								return false;
					} else if (!add(SmoothPair(array->Elements)))
						return false;
				} else if (const auto tuple =
							   std::visit([](const auto &leaf) { return SmoothTuple(leaf); }, *value)) {
					if (!add(tuple)) return false;
				} else if (context.IsLinked(input.Id)) {
					const auto number = SmoothNumber(*value);
					if (number && !add(Vector2{*number, *number})) return false;
				}
			}
			return true;
		}
		bool PreviousSmoothPosition(NodeContext &context, Vector2 &position) {
			const auto *owner = context.CurrentData ? context.CurrentData : context.Request.DataReplay;
			if (!owner) return true;
			Diagnostic diagnostic;
			if (ValidateDataReplay(*owner, context.ByteBudget, diagnostic) != Status::Ok)
				return context.Fail(diagnostic);
			for (const auto &entry : owner->Entries) {
				if (entry.NodeId != context.Authored.Id || entry.ProcessorRow != context.ProcessorRow)
					continue;
				const auto time = FrameTimeToReal({entry.Tick, entry.Subframe, entry.NegativeFrame});
				const auto current = FrameTimeToReal(
					{context.Request.Tick, context.Request.Subframe, context.Request.NegativeFrame}
				);
				if (!entry.Initialized || time > current || entry.Values.size() != 1 ||
					entry.Values[0].Frame != entry.Tick)
					return context.Fail(
						Status::InvalidValue, "Smooth Path position replay receipt is invalid", "position_out"
					);
				const auto *previous = std::get_if<Vector2>(&entry.Values[0].Data);
				if (!previous || !std::isfinite(previous->X) || !std::isfinite(previous->Y))
					return context.Fail(
						Status::InvalidValue, "Smooth Path position replay payload is invalid", "position_out"
					);
				position = *previous;
				return true;
			}
			return true;
		}
		bool SourceSmoothPath(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.source.path_smooth");
			const bool loop = context.Boolean("loop"), round = context.Boolean("round_anchor"),
					   normalized = context.Boolean("normalized_length", true);
			const double smoothness = context.Scalar("smoothness", 3), sample = context.Scalar("sample_path"),
						 mode = context.SourceChoice("sample_mode");
			if (context.FailureCode != Status::Ok) return false;
			if (!std::isfinite(smoothness) || !std::isfinite(sample) || !std::isfinite(mode))
				return context.Fail(
					Status::InvalidValue, "Smooth Path controls must be finite", "smoothness"
				);
			Vector2 position{};
			if (!PreviousSmoothPosition(context, position)) return false;
			size_t count = 0;
			if (!CollectSmoothAnchors(context, [&](Vector2, std::string_view port) {
					if (count >= Limits::MaximumPathAnchors)
						return context.Fail(Status::LimitExceeded, "Smooth Path exceeds anchor bounds", port);
					++count;
					return true;
				}))
				return false;
			const uint64_t bytes = count * sizeof(PathAnchor) + sizeof(SourceSmoothPathPolicy) +
								   sizeof(DataReplayEntry) + sizeof(DataReplayValueFrame) +
								   std::max(context.Authored.Id.size(), std::string{}.capacity()) +
								   2 * std::string{}.capacity();
			if (!context.ReserveOutput(bytes, "path_data")) return false;
			Path2D output;
			output.Loop = loop;
			output.SourceSmooth.emplace().NormalizedLength = normalized;
			output.Anchors.reserve(count);
			if (!CollectSmoothAnchors(context, [&](Vector2 point, std::string_view) {
					if (round) point = {DriverRoundHalfEven(point.X), DriverRoundHalfEven(point.Y)};
					PathAnchor anchor;
					anchor.Controls[0] = point.X;
					anchor.Controls[1] = point.Y;
					output.Anchors.push_back(anchor);
					return true;
				}))
				return false;
			if (count != 2) {
				for (size_t index = 0; index < count; ++index) {
					// Source clears open endpoint handles after calculating them.
					if (!loop && (index == 0 || index + 1 == count)) continue;
					const auto &before = output.Anchors[(index + count - 1) % count].Controls,
							   &after = output.Anchors[(index + 1) % count].Controls;
					auto &anchor = output.Anchors[index].Controls;
					const double direction =
									 SourceWeightDirection(after[0] - before[0], after[1] - before[1]) *
									 std::numbers::pi / 180,
								 inDistance =
									 std::hypot(anchor[0] - before[0], anchor[1] - before[1]) / smoothness,
								 outDistance =
									 std::hypot(anchor[0] - after[0], anchor[1] - after[1]) / smoothness;
					anchor[2] = -SourceShapeLengthdirComponent(inDistance * std::cos(direction));
					anchor[3] = -SourceShapeLengthdirComponent(-inDistance * std::sin(direction));
					anchor[4] = SourceShapeLengthdirComponent(outDistance * std::cos(direction));
					anchor[5] = SourceShapeLengthdirComponent(-outDistance * std::sin(direction));
					if (!std::all_of(anchor.begin(), anchor.end(), [](double component) {
							return std::isfinite(component);
						}))
						return context.Fail(
							Status::InvalidValue, "Smooth Path control calculation is nonfinite", "smoothness"
						);
				}
			}
			if (!ValidSourcePath2D(output))
				return context.Fail(Status::InvalidValue, "Smooth Path output is invalid", "path_data");
			// Initial source position sampling seeds the same cache later consumers share.
			if (!StampSourcePathShiftOutput(context, output)) return false;
			PathRuntime runtime;
			if (!runtime.Init(context, output)) return false;
			if (count && (mode == 0 || mode == 1)) {
				const auto point = mode == 0 ? runtime.PointRatio(sample) : runtime.PointSegment(sample);
				position = {point.X, point.Y};
			}
			if (context.FailureCode != Status::Ok) return false;
			if (!std::isfinite(position.X) || !std::isfinite(position.Y) || !std::isfinite(runtime.Length()))
				return context.Fail(
					Status::InvalidValue, "Smooth Path sample or length is nonfinite", "sample_path"
				);
			DataReplayEntry receipt;
			receipt.NodeId = context.Authored.Id;
			receipt.ProcessorRow = context.ProcessorRow;
			receipt.Tick = context.Request.Tick;
			receipt.Subframe = context.Request.Subframe;
			receipt.NegativeFrame = context.Request.NegativeFrame;
			receipt.Initialized = true;
			receipt.Values.reserve(1);
			receipt.Values.push_back({context.Request.Tick, Value{position}});
			context.DataUpdates.reserve(context.DataUpdates.size() + 1);
			context.SetValue("path_data", std::move(output));
			context.SetValue("position_out", position);
			if (context.FailureCode != Status::Ok) return false;
			context.DataUpdates.push_back(std::move(receipt));
			return true;
		}
	}
	std::span<const ExecutorEntry> SourceSmoothPathExecutors() {
		static constexpr std::array ENTRIES{ExecutorEntry{"pc.path_smooth", SourceSmoothPath, true}};
		return ENTRIES;
	}
}
