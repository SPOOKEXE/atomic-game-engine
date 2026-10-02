// Path family executors built on the node_path.gml path runtime.

#include "Families.hpp"
#include "Path.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace engine::imagegraph::detail {
	namespace {
		using SourceAnchor = std::array<double, 7>;
		using OrderedAnchor = std::pair<size_t, SourceAnchor>;

		// node_path.gml update: builds the path from its anchors and samples one point on it.
		bool Path(NodeContext &context) {
			size_t anchorCount = 0;
			for (const DynamicInput &input : context.Authored.DynamicInputs) {
				size_t group = 0;
				const CatalogueInput *slot = FindDynamicTemplate(context.Entry, input.Id, group);
				if (slot && slot->Id == "anchor") anchorCount++;
			}
			const auto *storedWeights = std::get_if<ArrayValue>(context.Find("attribute_weight"));
			size_t weightCount = 4;
			if (storedWeights) {
				weightCount = 0;
				for (const ElementValue &element : storedWeights->Elements)
					if (std::holds_alternative<double>(element)) weightCount++;
			}
			if (anchorCount > Limits::MaximumPathAnchors || anchorCount > Limits::MaximumArrayElements / 7 ||
				weightCount > Limits::MaximumArrayElements || weightCount / 2 > Limits::MaximumPathWeights)
				return context.Fail(
					Status::LimitExceeded, "path exceeds the anchor or weight limit", "path_data"
				);
			const uint64_t names = std::max<size_t>(12, std::string{}.capacity()) +
								   std::max<size_t>(9, std::string{}.capacity()) +
								   std::max<size_t>(7, std::string{}.capacity()) +
								   std::max<size_t>(7, std::string{}.capacity());
			const uint64_t outputBytes = anchorCount * sizeof(PathAnchor) +
										 (weightCount / 2) * sizeof(PathWeight) +
										 (anchorCount * 7 + weightCount) * sizeof(ElementValue) + names;
			if (!context.ReserveOutput(outputBytes, "path_data")) return false;
			auto orderedCharge = context.ReserveWorkspace(anchorCount * sizeof(OrderedAnchor), "anchors");
			if (!orderedCharge) return false;
			std::vector<OrderedAnchor> ordered;
			ordered.reserve(anchorCount);
			for (const DynamicInput &input : context.Authored.DynamicInputs) {
				size_t group = 0;
				const CatalogueInput *slot = FindDynamicTemplate(context.Entry, input.Id, group);
				if (!slot || slot->Id != "anchor") continue;
				SourceAnchor anchor{};
				if (const auto *array = std::get_if<ArrayValue>(context.Find(input.Id)))
					for (size_t index = 0; index < std::min<size_t>(7, array->Elements.size()); index++)
						if (const auto *number = std::get_if<double>(&array->Elements[index]))
							anchor[index] = *number;
				ordered.emplace_back(group, anchor);
			}
			std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
				return a.first < b.first;
			});
			if (context.Boolean("round_anchor"))
				for (auto &[group, anchor] : ordered) {
					anchor[0] = std::nearbyint(anchor[0]);
					anchor[1] = std::nearbyint(anchor[1]);
				}
			Path2D path;
			path.Loop = context.Boolean("loop");
			path.Anchors.reserve(anchorCount);
			path.Weights.reserve(weightCount / 2);
			ArrayValue anchors{ValueType::Scalar, {}}, weights{ValueType::Scalar, {}};
			anchors.Elements.reserve(anchorCount * 7);
			weights.Elements.reserve(weightCount);
			for (const auto &[group, anchor] : ordered) {
				PathAnchor stored;
				std::copy_n(anchor.begin(), 6, stored.Controls.begin());
				stored.Index = static_cast<int64_t>(anchor[6]);
				path.Anchors.push_back(stored);
				for (const double number : anchor)
					anchors.Elements.emplace_back(number);
			}
			if (storedWeights) {
				for (const ElementValue &element : storedWeights->Elements)
					if (const auto *number = std::get_if<double>(&element))
						weights.Elements.emplace_back(*number);
			} else
				for (const double number : {0.0, 1.0, 100.0, 1.0})
					weights.Elements.emplace_back(number);
			for (size_t index = 0; index + 1 < weights.Elements.size(); index += 2)
				path.Weights.push_back(
					{std::get<double>(weights.Elements[index]), std::get<double>(weights.Elements[index + 1])}
				);
			PathRuntime runtime;
			if (!runtime.Init(context, path)) return false;
			const double ratio = context.Scalar("sample_path");
			const PathPoint point =
				context.Integer("sample_mode") == 0 ? runtime.PointRatio(ratio) : runtime.PointSegment(ratio);
			if (context.FailureCode != Status::Ok) return false;
			context.SetValue("position_out", Vector2{point.X, point.Y});
			context.SetValue("path_data", std::move(path));
			context.SetValue("anchors", std::move(anchors));
			context.SetValue("weights", std::move(weights));
			return context.FailureCode == Status::Ok;
		}

		constexpr ExecutorEntry PATH_EXECUTORS[] = {
			{"pc.path", Path},
		};
	}

	std::span<const ExecutorEntry> PathExecutors() {
		return PATH_EXECUTORS;
	}
}
